/* anywin runtime - advapi32 registry (S8).
 *
 * The registry is a set of files under ~/.anywin/registry/, one per predefined root
 * (hkcr.reg, hkcu.reg, hklm.reg, hku.reg, hkcc.reg). Each file is a line-oriented text file:
 *   K<TAB><key path>                      a key (path relative to the root, case preserved)
 *   V<TAB><key path><TAB><name><TAB><type><TAB><hex data>
 * Fields are escaped (\\, \t, \n, \r, \xHH). Keys and value names compare case-insensitively.
 * Every call takes a process-wide mutex and an flock() on the directory's lock file, reloads the
 * hive it touches and rewrites it through a temporary file and rename(), so concurrent processes
 * see each other's changes. String values are stored as UTF-16LE with a terminator (REG_SZ,
 * EXPAND_SZ, MULTI_SZ); the A functions convert them to and from UTF-8.
 *
 * Writing a string value under HKCU\Software\Microsoft\Windows\CurrentVersion\Run also writes
 * ~/.config/autostart/<value>.desktop; deleting the value (or the key) removes it. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 2
#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_PARAMETER 87
#define ERR_MORE_DATA 234
#define ERR_NO_MORE_ITEMS 259
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define REG_CREATED_NEW_KEY 1
#define REG_OPENED_EXISTING_KEY 2
#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* predefined roots: the index is the position in root_names[] */
enum { ROOT_CR, ROOT_CU, ROOT_LM, ROOT_U, ROOT_CC, ROOT_COUNT };
static const char *const root_names[ROOT_COUNT] = {"hkcr", "hkcu", "hklm", "hku", "hkcc"};

struct regkey {
    int root;
    char *path;
    struct regkey *next;
};
static struct regkey *open_keys; /* handles returned by RegOpenKeyEx / RegCreateKeyEx */

/* ---- locking and storage directory -------------------------------------- */
static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;
static int reg_lock_fd = -1;
static char *reg_dir; /* ~/.anywin/registry, set on first use */

static void reg_begin(void)
{
    pthread_mutex_lock(&reg_mu);
    if (!reg_dir) {
        const char *home = getenv("HOME");
        struct passwd *pw = home && *home ? NULL : getpwuid(getuid());
        if (!home || !*home)
            home = pw ? pw->pw_dir : "/tmp";
        char *base = NULL, *cfg = NULL;
        if (asprintf(&base, "%s/.anywin", home) < 0 || asprintf(&reg_dir, "%s/registry", base) < 0)
            reg_dir = NULL;
        else {
            mkdir(base, 0700);
            mkdir(reg_dir, 0700);
            if (asprintf(&cfg, "%s/.lock", reg_dir) >= 0)
                reg_lock_fd = open(cfg, O_RDWR | O_CREAT, 0600);
        }
        free(base);
        free(cfg);
        if (!reg_dir)
            reg_dir = strdup("/tmp");
    }
    if (reg_lock_fd >= 0)
        flock(reg_lock_fd, LOCK_EX);
}

static void reg_end(void)
{
    if (reg_lock_fd >= 0)
        flock(reg_lock_fd, LOCK_UN);
    pthread_mutex_unlock(&reg_mu);
}

/* ---- paths ------------------------------------------------------------------ */
/* base + "\" + sub, skipping empty components; 0 when it does not fit */
static int path_join(const char *base, const char *sub, char *out, size_t cap)
{
    size_t n = snprintf(out, cap, "%s", base ? base : "");
    if (n >= cap)
        return 0;
    for (const char *p = sub; p && *p;) {
        while (*p == '\\')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != '\\')
            e++;
        size_t len = (size_t)(e - p);
        if (n + 1 + len + 1 > cap)
            return 0;
        if (n)
            out[n++] = '\\';
        memcpy(out + n, p, len);
        n += len;
        out[n] = 0;
        p = e;
    }
    return 1;
}

static const char *leaf_of(const char *path)
{
    const char *s = strrchr(path, '\\');
    return s ? s + 1 : path;
}

/* does `key` sit directly under `parent` ("" is the root)? */
static int is_child_of(const char *key, const char *parent)
{
    size_t pl = strlen(parent);
    const char *last = strrchr(key, '\\');
    size_t kl = last ? (size_t)(last - key) : 0;
    return kl == pl && strncasecmp(key, parent, pl) == 0;
}

/* ---- escaping and hex ----------------------------------------------------------- */
static void put_escaped(FILE *f, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') fputs("\\\\", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c < 0x20 || c == 0x7f) fprintf(f, "\\x%02x", c);
        else fputc(c, f);
    }
}

/* decodes an escaped field of length n into a malloc'd, nul-terminated string */
static char *get_unescaped(const char *s, size_t n)
{
    char *o = malloc(n + 1), *d = o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            *d++ = s[i];
            continue;
        }
        char c = s[++i];
        if (c == 't') *d++ = '\t';
        else if (c == 'n') *d++ = '\n';
        else if (c == 'r') *d++ = '\r';
        else if (c == 'x' && i + 2 < n) {
            char hx[3] = {s[i + 1], s[i + 2], 0};
            *d++ = (char)strtol(hx, NULL, 16);
            i += 2;
        } else *d++ = c;
    }
    *d = 0;
    return o;
}

static char *hex_encode(const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    char *o = malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
        o[2 * i] = digits[p[i] >> 4];
        o[2 * i + 1] = digits[p[i] & 15];
    }
    o[n * 2] = 0;
    return o;
}

static uint8_t *hex_decode(const char *s, size_t n, size_t *out_n)
{
    uint8_t *o = malloc(n / 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        char hx[3] = {s[i], s[i + 1], 0};
        o[k++] = (uint8_t)strtol(hx, NULL, 16);
    }
    *out_n = k;
    return o;
}

/* ---- hive contents ----------------------------------------------------------------- */
struct rkey {
    char *path;
    int builtin; /* predefined by the runtime: not written to the file */
};
struct rval {
    char *keypath, *name;
    DWORD type, size;
    uint8_t *data; /* string types: UTF-16LE with a terminator */
    int builtin;
};
struct hive {
    struct rkey *keys;
    int nkeys, capkeys;
    struct rval *vals;
    int nvals, capvals;
};

static int key_find(const struct hive *h, const char *path)
{
    for (int i = 0; i < h->nkeys; i++)
        if (!strcasecmp(h->keys[i].path, path))
            return i;
    return -1;
}

static int key_add(struct hive *h, const char *path, int builtin)
{
    if (h->nkeys == h->capkeys) {
        h->capkeys = h->capkeys ? h->capkeys * 2 : 32;
        h->keys = realloc(h->keys, h->capkeys * sizeof *h->keys);
    }
    h->keys[h->nkeys].path = strdup(path);
    h->keys[h->nkeys].builtin = builtin;
    return h->nkeys++;
}

static int val_find(const struct hive *h, const char *keypath, const char *name)
{
    for (int i = 0; i < h->nvals; i++)
        if (!strcasecmp(h->vals[i].keypath, keypath) && !strcasecmp(h->vals[i].name, name))
            return i;
    return -1;
}

/* stores (or replaces) a value; takes ownership of data */
static void val_put(struct hive *h, const char *keypath, const char *name, DWORD type, uint8_t *data, DWORD size,
                    int builtin)
{
    int i = val_find(h, keypath, name);
    if (i < 0) {
        if (h->nvals == h->capvals) {
            h->capvals = h->capvals ? h->capvals * 2 : 32;
            h->vals = realloc(h->vals, h->capvals * sizeof *h->vals);
        }
        i = h->nvals++;
        h->vals[i].keypath = strdup(keypath);
        h->vals[i].name = strdup(name);
    } else {
        free(h->vals[i].data);
    }
    h->vals[i].type = type;
    h->vals[i].data = data;
    h->vals[i].size = size;
    h->vals[i].builtin = builtin;
}

static void val_remove(struct hive *h, int i)
{
    free(h->vals[i].keypath);
    free(h->vals[i].name);
    free(h->vals[i].data);
    h->vals[i] = h->vals[--h->nvals];
}

static void key_remove(struct hive *h, int i)
{
    free(h->keys[i].path);
    h->keys[i] = h->keys[--h->nkeys];
}

static void hive_free(struct hive *h)
{
    for (int i = 0; i < h->nkeys; i++)
        free(h->keys[i].path);
    for (int i = 0; i < h->nvals; i++) {
        free(h->vals[i].keypath);
        free(h->vals[i].name);
        free(h->vals[i].data);
    }
    free(h->keys);
    free(h->vals);
    memset(h, 0, sizeof *h);
}

static int has_children(const struct hive *h, const char *path)
{
    for (int i = 0; i < h->nkeys; i++)
        if (is_child_of(h->keys[i].path, path))
            return 1;
    return 0;
}

/* ---- value data: stored form and the A (UTF-8) form ----------------------------- */
/* Converts the caller's bytes into the stored form. Strings stop at their first nul (a
 * MULTI_SZ at its double nul) and are stored UTF-16LE with the terminators. */
static uint8_t *to_stored(DWORD type, const uint8_t *in, DWORD cb, DWORD *out_size)
{
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        size_t n = in ? strnlen((const char *)in, cb) : 0;
        char *tmp = strndup(in ? (const char *)in : "", n);
        WCHAR *w = aw_u2w(tmp, -1);
        free(tmp);
        size_t wl = aw_wcslen(w);
        uint8_t *o = malloc((wl + 1) * sizeof(WCHAR));
        memcpy(o, w, wl * sizeof(WCHAR));
        ((WCHAR *)o)[wl] = 0;
        free(w);
        *out_size = (DWORD)((wl + 1) * sizeof(WCHAR));
        return o;
    }
    if (type == REG_MULTI_SZ) {
        size_t cap = 64, used = 0;
        uint8_t *o = malloc(cap);
        for (DWORD pos = 0; in && pos < cb;) {
            size_t seg = strnlen((const char *)in + pos, cb - pos);
            if (!seg)
                break;
            char *tmp = strndup((const char *)in + pos, seg);
            WCHAR *w = aw_u2w(tmp, -1);
            free(tmp);
            size_t need = (aw_wcslen(w) + 1) * sizeof(WCHAR);
            if (used + need + sizeof(WCHAR) > cap) {
                cap = (used + need + sizeof(WCHAR)) * 2;
                o = realloc(o, cap);
            }
            memcpy(o + used, w, need);
            used += need;
            free(w);
            pos += (DWORD)seg + 1;
        }
        if (used + sizeof(WCHAR) > cap)
            o = realloc(o, used + sizeof(WCHAR));
        ((WCHAR *)(o + used))[0] = 0; /* list terminator */
        *out_size = (DWORD)(used + sizeof(WCHAR));
        return o;
    }
    uint8_t *o = malloc(cb ? cb : 1);
    if (cb && in)
        memcpy(o, in, cb);
    *out_size = cb;
    return o;
}

/* Converts the stored form to the A form a caller receives (a malloc'd buffer). */
static uint8_t *from_stored(DWORD type, const uint8_t *st, DWORD sz, DWORD *out_size)
{
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        const WCHAR *w = (const WCHAR *)st;
        size_t wl = 0;
        while (wl < sz / sizeof(WCHAR) && w[wl])
            wl++;
        char *u = aw_w2u(w, (int)wl);
        *out_size = (DWORD)strlen(u) + 1;
        return (uint8_t *)u;
    }
    if (type == REG_MULTI_SZ) {
        size_t cap = 64, used = 0;
        uint8_t *o = malloc(cap);
        const WCHAR *w = (const WCHAR *)st;
        for (size_t i = 0; i < sz / sizeof(WCHAR) && w[i];) {
            size_t wl = 0;
            while (i + wl < sz / sizeof(WCHAR) && w[i + wl])
                wl++;
            char *u = aw_w2u(w + i, (int)wl);
            size_t n = strlen(u) + 1;
            if (used + n + 1 > cap) {
                cap = (used + n + 1) * 2;
                o = realloc(o, cap);
            }
            memcpy(o + used, u, n);
            used += n;
            free(u);
            i += wl + 1;
        }
        o[used] = 0;
        *out_size = (DWORD)(used + 1);
        return o;
    }
    uint8_t *o = malloc(sz ? sz : 1);
    if (sz)
        memcpy(o, st, sz);
    *out_size = sz;
    return o;
}

/* ---- hive files ----------------------------------------------------------------- */
static char *hive_path(int root)
{
    char *p = NULL;
    if (asprintf(&p, "%s/%s.reg", reg_dir, root_names[root]) < 0)
        p = NULL;
    return p;
}

/* keys that exist on every Windows install; they are kept in memory, not written to the file */
static void seed_keys(struct hive *h, const char *const *keys, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (key_find(h, keys[i]) < 0)
            key_add(h, keys[i], 1);
}

static void seed_hkcu(struct hive *h)
{
    static const char *const keys[] = {"Software", "Software\\Microsoft", "Software\\Microsoft\\Windows",
                                       "Software\\Microsoft\\Windows\\CurrentVersion", RUN_KEY};
    seed_keys(h, keys, sizeof keys / sizeof *keys);
}

static void seed_hklm(struct hive *h)
{
    static const char *const keys[] = {"Software", "Software\\Microsoft", "Software\\Microsoft\\Windows NT",
                                       "Software\\Microsoft\\Windows NT\\CurrentVersion"};
    seed_keys(h, keys, sizeof keys / sizeof *keys);
    const char *cv = "Software\\Microsoft\\Windows NT\\CurrentVersion";
    static const struct {
        const char *name, *text;
    } strs[] = {{"ProductName", "Windows 10 Pro"},  {"EditionID", "Professional"},
                {"CurrentBuild", "19045"},          {"CurrentBuildNumber", "19045"},
                {"CurrentVersion", "6.3"},          {"InstallationType", "Client"}};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) {
        if (val_find(h, cv, strs[i].name) >= 0)
            continue;
        DWORD sz;
        uint8_t *d = to_stored(REG_SZ, (const uint8_t *)strs[i].text, (DWORD)strlen(strs[i].text), &sz);
        val_put(h, cv, strs[i].name, REG_SZ, d, sz, 1);
    }
    static const struct {
        const char *name;
        DWORD value;
    } dwords[] = {{"CurrentMajorVersionNumber", 10}, {"CurrentMinorVersionNumber", 0}};
    for (size_t i = 0; i < sizeof dwords / sizeof *dwords; i++) {
        if (val_find(h, cv, dwords[i].name) >= 0)
            continue;
        uint8_t *d = malloc(4);
        memcpy(d, &dwords[i].value, 4);
        val_put(h, cv, dwords[i].name, REG_DWORD, d, 4, 1);
    }
}

static void hive_load(struct hive *h, int root)
{
    memset(h, 0, sizeof *h);
    char *file = hive_path(root);
    FILE *f = file ? fopen(file, "rb") : NULL;
    free(file);
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(n + 1);
        size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        buf[got] = 0;
        for (char *line = buf; line && *line;) {
            char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            if (len && line[0] == 'K' && line[1] == '\t') {
                char *p = get_unescaped(line + 2, len - 2);
                if (key_find(h, p) < 0)
                    key_add(h, p, 0);
                free(p);
            } else if (len && line[0] == 'V' && line[1] == '\t') {
                /* V <kp> <name> <type> <hex>: four tab-separated fields */
                const char *fld[4];
                size_t flen[4];
                int nf = 0;
                const char *s = line + 2, *end = line + len;
                fld[nf] = s;
                while (nf < 3 && s < end) {
                    if (*s == '\t') {
                        flen[nf] = (size_t)(s - fld[nf]);
                        fld[++nf] = s + 1;
                    }
                    s++;
                }
                if (nf == 3) {
                    flen[3] = (size_t)(end - fld[3]);
                    char *kp = get_unescaped(fld[0], flen[0]);
                    char *nm = get_unescaped(fld[1], flen[1]);
                    DWORD type = (DWORD)strtoul(fld[2], NULL, 10);
                    size_t dn;
                    uint8_t *d = hex_decode(fld[3], flen[3], &dn);
                    if (key_find(h, kp) < 0)
                        key_add(h, kp, 0);
                    val_put(h, kp, nm, type, d, (DWORD)dn, 0);
                    free(kp);
                    free(nm);
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        free(buf);
    }
    if (root == ROOT_CU)
        seed_hkcu(h);
    if (root == ROOT_LM)
        seed_hklm(h);
}

/* writes the hive to a temporary file and renames it over the old one */
static DWORD hive_save(const struct hive *h, int root)
{
    char *file = hive_path(root), *tmp = NULL;
    if (!file || asprintf(&tmp, "%s.tmp.%ld", file, (long)getpid()) < 0) {
        free(file);
        return ERR_ACCESS_DENIED;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        DWORD e = aw_errno_to_win(errno);
        free(file);
        free(tmp);
        return e;
    }
    fprintf(f, "# anywin registry v1\n");
    for (int i = 0; i < h->nkeys; i++) {
        if (h->keys[i].builtin)
            continue;
        fputs("K\t", f);
        put_escaped(f, h->keys[i].path, strlen(h->keys[i].path));
        fputc('\n', f);
    }
    for (int i = 0; i < h->nvals; i++) {
        const struct rval *v = &h->vals[i];
        if (v->builtin)
            continue;
        fputs("V\t", f);
        put_escaped(f, v->keypath, strlen(v->keypath));
        fputc('\t', f);
        put_escaped(f, v->name, strlen(v->name));
        fprintf(f, "\t%u\t", (unsigned)v->type);
        char *hx = hex_encode(v->data, v->size);
        fputs(hx, f);
        free(hx);
        fputc('\n', f);
    }
    int bad = fflush(f) || ferror(f);
    if (!bad) {
        int fd = fileno(f);
        fsync(fd);
    }
    DWORD rc = ERR_SUCCESS;
    if (fclose(f) || bad) {
        rc = aw_errno_to_win(errno ? errno : EIO);
        unlink(tmp);
    } else if (rename(tmp, file)) {
        rc = aw_errno_to_win(errno);
        unlink(tmp);
    }
    free(file);
    free(tmp);
    return rc;
}

/* ---- autostart (XDG) ----------------------------------------------------------------- */
/* creates every component of an absolute directory path */
static int mkdir_p(const char *path)
{
    char *buf = strdup(path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    int ok = mkdir(buf, 0755) == 0 || errno == EEXIST;
    free(buf);
    return ok;
}

/* ~/.config/autostart/<name>.desktop (malloc'd); '/' in the name is replaced */
static char *autostart_path(const char *name)
{
    char *safe = malloc(strlen(name) + 1);
    size_t n = 0;
    for (const char *p = name; *p; p++, n++)
        safe[n] = (*p == '/' || *p == '\n' || *p == '\r') ? '_' : *p;
    safe[n] = 0;
    if (!n) {
        free(safe);
        safe = strdup("default");
    }
    const char *home = getenv("HOME");
    char *out = NULL;
    if (asprintf(&out, "%s/.config/autostart/%s.desktop", home && *home ? home : "/tmp", safe) < 0)
        out = NULL;
    free(safe);
    return out;
}

static void autostart_remove(const char *name)
{
    char *p = autostart_path(name);
    if (p)
        unlink(p);
    free(p);
}

/* appends `s` to an Exec= line, quoting it when it has characters the shell would split on */
static void exec_append(char **buf, size_t *len, size_t *cap, const char *s)
{
    int quote = !*s;
    for (const char *p = s; *p && !quote; p++)
        quote = strchr(" \t\"'\\$`;&|<>()*?#~%", *p) != NULL;
    size_t need = strlen(s) * 2 + 4 + *len + 1;
    if (need > *cap) {
        *cap = need * 2;
        *buf = realloc(*buf, *cap);
    }
    char *o = *buf + *len;
    if (quote)
        *o++ = '"';
    for (const char *p = s; *p; p++) {
        if (quote && strchr("\"`$\\", *p))
            *o++ = '\\';
        if (*p == '%')
            *o++ = '%';
        *o++ = *p;
    }
    if (quote)
        *o++ = '"';
    *o = 0;
    *len = (size_t)(o - *buf);
}

/* the command line `cmd` (UTF-8, Windows syntax) becomes the autostart entry */
static void autostart_write(const char *name, const char *cmd)
{
    /* split the command into tokens: "quoted" or space-separated */
    char **tok = NULL;
    int nt = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *s, *e;
        if (*p == '"') {
            s = p + 1;
            e = strchr(s, '"');
            if (!e)
                e = s + strlen(s);
            p = *e ? e + 1 : e;
        } else {
            s = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            e = p;
        }
        tok = realloc(tok, (nt + 1) * sizeof *tok);
        tok[nt++] = strndup(s, (size_t)(e - s));
    }
    char *exec = NULL;
    size_t len = 0, cap = 0;
    if (nt) {
        const char *prog = tok[0];
        char *unixp = !strcasecmp(prog, aw_exe_win) ? strdup(aw_exe_unix) : aw_unix_path(prog);
        exec_append(&exec, &len, &cap, unixp ? unixp : prog);
        free(unixp);
        for (int i = 1; i < nt; i++) {
            if (len + 1 > cap) { cap = (len + 2) * 2; exec = realloc(exec, cap); }
            exec[len++] = ' ';
            exec[len] = 0;
            exec_append(&exec, &len, &cap, tok[i]);
        }
    }
    for (int i = 0; i < nt; i++)
        free(tok[i]);
    free(tok);
    if (!exec) {
        autostart_remove(name);
        return;
    }

    const char *home = getenv("HOME");
    char *dir = NULL, *file = autostart_path(name), *tmp = NULL;
    if (asprintf(&dir, "%s/.config/autostart", home && *home ? home : "/tmp") >= 0)
        mkdir_p(dir);
    free(dir);
    if (file && asprintf(&tmp, "%s.tmp.%ld", file, (long)getpid()) >= 0) {
        char *nm = strdup(name);
        for (char *c = nm; *c; c++)
            if (*c == '\n' || *c == '\r')
                *c = ' ';
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nHidden=false\n"
                       "X-GNOME-Autostart-enabled=true\n", *nm ? nm : "default", exec);
            if (fclose(f) == 0)
                rename(tmp, file);
            else
                unlink(tmp);
        }
        free(nm);
    }
    free(tmp);
    free(file);
    free(exec);
}

/* ---- handles -------------------------------------------------------------------------- */
/* Predefined roots are 0x80000000..0x80000005 (HKEY_PERFORMANCE_DATA is not provided). The MinGW
 * headers and 64-bit Windows sign-extend them, so both forms are accepted. -1 for other values. */
static int predefined_root(HKEY h)
{
    uint64_t v = (uint64_t)(uintptr_t)h;
    if ((v >> 32) != 0 && (v >> 32) != 0xffffffffu)
        return -1;
    uint32_t lo = (uint32_t)v;
    if (lo == 0x80000005u)
        return ROOT_CC;
    if (lo >= 0x80000000u && lo <= 0x80000003u)
        return (int)(lo - 0x80000000u);
    return -1;
}

/* Caller holds the registry lock. Returns the root index and the key path, or -1. */
static int key_resolve(HKEY h, char *path, size_t cap)
{
    int root = predefined_root(h);
    if (root >= 0) {
        if (path)
            path[0] = 0;
        return root;
    }
    for (struct regkey *k = open_keys; k; k = k->next)
        if ((void *)k == (void *)h) {
            snprintf(path, cap, "%s", k->path);
            return k->root;
        }
    return -1;
}

static HKEY new_key_handle(int root, const char *path)
{
    struct regkey *k = calloc(1, sizeof *k);
    k->root = root;
    k->path = strdup(path);
    k->next = open_keys;
    open_keys = k;
    return (HKEY)k;
}

static int is_run_key(int root, const char *path) { return root == ROOT_CU && !strcasecmp(path, RUN_KEY); }

/* ---- API ------------------------------------------------------------------------------------ */
API(advapi32, LONG, RegCloseKey, (HKEY hkey))
{
    if (predefined_root(hkey) >= 0)
        return ERR_SUCCESS;
    reg_begin();
    LONG rc = ERR_INVALID_HANDLE;
    for (struct regkey **pp = &open_keys; *pp; pp = &(*pp)->next)
        if ((void *)*pp == (void *)hkey) {
            struct regkey *k = *pp;
            *pp = k->next;
            free(k->path);
            free(k);
            rc = ERR_SUCCESS;
            break;
        }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegOpenKeyExA, (HKEY hkey, LPCSTR sub, DWORD opts, DWORD sam, HKEY *out))
{
    (void)opts;
    (void)sam;
    if (!out)
        return ERR_INVALID_PARAMETER;
    char base[4096], path[4096];
    reg_begin();
    int root = key_resolve(hkey, base, sizeof base);
    LONG rc = ERR_SUCCESS;
    if (root < 0)
        rc = ERR_INVALID_HANDLE;
    else if (!path_join(base, sub, path, sizeof path))
        rc = ERR_INVALID_PARAMETER;
    else if (path[0]) {
        struct hive h;
        hive_load(&h, root);
        if (key_find(&h, path) < 0)
            rc = ERR_FILE_NOT_FOUND;
        hive_free(&h);
    }
    if (rc == ERR_SUCCESS)
        *out = new_key_handle(root, path);
    reg_end();
    return rc;
}

API(advapi32, LONG, RegCreateKeyExA, (HKEY hkey, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD opts, DWORD sam,
                                     void *secattr, HKEY *out, DWORD *disp))
{
    (void)reserved;
    (void)cls;
    (void)opts;
    (void)sam;
    (void)secattr;
    if (!out)
        return ERR_INVALID_PARAMETER;
    char base[4096], path[4096];
    reg_begin();
    int root = key_resolve(hkey, base, sizeof base);
    LONG rc = ERR_SUCCESS;
    DWORD how = REG_OPENED_EXISTING_KEY;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else if (!path_join(base, sub, path, sizeof path)) {
        rc = ERR_INVALID_PARAMETER;
    } else {
        struct hive h;
        hive_load(&h, root);
        int changed = 0, leaf_existed = 1;
        /* walk the components of sub, creating the missing ones; the disposition is about the last */
        char cur[4096], next[4096];
        snprintf(cur, sizeof cur, "%s", base);
        for (const char *p = sub ? sub : ""; *p;) {
            while (*p == '\\')
                p++;
            if (!*p)
                break;
            const char *e = p;
            while (*e && *e != '\\')
                e++;
            char comp[512];
            size_t cl = (size_t)(e - p);
            if (cl >= sizeof comp) {
                rc = ERR_INVALID_PARAMETER;
                break;
            }
            memcpy(comp, p, cl);
            comp[cl] = 0;
            if (!path_join(cur, comp, next, sizeof next)) {
                rc = ERR_INVALID_PARAMETER;
                break;
            }
            leaf_existed = key_find(&h, next) >= 0;
            if (!leaf_existed) {
                key_add(&h, next, 0);
                changed = 1;
            }
            snprintf(cur, sizeof cur, "%s", next);
            p = e;
        }
        if (rc == ERR_SUCCESS) {
            snprintf(path, sizeof path, "%s", cur);
            how = leaf_existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
            if (changed)
                rc = hive_save(&h, root);
        }
        hive_free(&h);
    }
    if (rc == ERR_SUCCESS) {
        *out = new_key_handle(root, path);
        if (disp)
            *disp = how;
    }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegDeleteKeyA, (HKEY hkey, LPCSTR sub))
{
    if (!sub || !*sub)
        return ERR_INVALID_PARAMETER;
    char base[4096], path[4096];
    reg_begin();
    int root = key_resolve(hkey, base, sizeof base);
    LONG rc = ERR_SUCCESS;
    if (root < 0)
        rc = ERR_INVALID_HANDLE;
    else if (!path_join(base, sub, path, sizeof path))
        rc = ERR_INVALID_PARAMETER;
    else {
        struct hive h;
        hive_load(&h, root);
        int k = key_find(&h, path);
        if (k < 0)
            rc = ERR_FILE_NOT_FOUND;
        else if (h.keys[k].builtin)
            rc = ERR_ACCESS_DENIED;
        else if (has_children(&h, path))
            rc = ERR_ACCESS_DENIED; /* RegDeleteKey never removes a key with subkeys */
        else {
            if (is_run_key(root, path))
                for (int i = 0; i < h.nvals; i++)
                    if (!strcasecmp(h.vals[i].keypath, path))
                        autostart_remove(h.vals[i].name);
            for (int i = h.nvals - 1; i >= 0; i--)
                if (!strcasecmp(h.vals[i].keypath, path))
                    val_remove(&h, i);
            key_remove(&h, k);
            rc = hive_save(&h, root);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegSetValueExA, (HKEY hkey, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD cb))
{
    (void)reserved;
    if (cb && !data)
        return ERR_INVALID_PARAMETER;
    char path[4096];
    reg_begin();
    int root = key_resolve(hkey, path, sizeof path);
    LONG rc = ERR_SUCCESS;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else {
        struct hive h;
        hive_load(&h, root);
        const char *vn = name ? name : "";
        if (path[0] && key_find(&h, path) < 0)
            rc = ERR_FILE_NOT_FOUND;
        else {
            int vi = val_find(&h, path, vn);
            if (vi >= 0 && h.vals[vi].builtin)
                rc = ERR_ACCESS_DENIED;
        }
        if (rc == ERR_SUCCESS) {
            DWORD sz;
            uint8_t *st = to_stored(type, data, cb, &sz);
            val_put(&h, path, vn, type, st, sz, 0);
            if (is_run_key(root, path)) {
                if (type == REG_SZ || type == REG_EXPAND_SZ) {
                    DWORD asz;
                    uint8_t *a = from_stored(type, st, sz, &asz);
                    autostart_write(vn, (const char *)a);
                    free(a);
                } else {
                    autostart_remove(vn);
                }
            }
            rc = hive_save(&h, root);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegQueryValueExA, (HKEY hkey, LPCSTR name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *cb))
{
    (void)reserved;
    char path[4096];
    reg_begin();
    int root = key_resolve(hkey, path, sizeof path);
    LONG rc = ERR_SUCCESS;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else if (data && !cb) {
        rc = ERR_INVALID_PARAMETER;
    } else {
        struct hive h;
        hive_load(&h, root);
        int vi = val_find(&h, path, name ? name : "");
        if (vi < 0) {
            rc = ERR_FILE_NOT_FOUND;
        } else {
            const struct rval *v = &h.vals[vi];
            DWORD outsz;
            uint8_t *out = from_stored(v->type, v->data, v->size, &outsz);
            if (type)
                *type = v->type;
            if (!data) {
                if (cb)
                    *cb = outsz;
            } else if (*cb < outsz) {
                *cb = outsz;
                rc = ERR_MORE_DATA;
            } else {
                memcpy(data, out, outsz);
                *cb = outsz;
            }
            free(out);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegDeleteValueA, (HKEY hkey, LPCSTR name))
{
    char path[4096];
    reg_begin();
    int root = key_resolve(hkey, path, sizeof path);
    LONG rc = ERR_SUCCESS;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else {
        struct hive h;
        hive_load(&h, root);
        const char *vn = name ? name : "";
        int vi = val_find(&h, path, vn);
        if (vi < 0)
            rc = ERR_FILE_NOT_FOUND;
        else if (h.vals[vi].builtin)
            rc = ERR_ACCESS_DENIED;
        else {
            val_remove(&h, vi);
            if (is_run_key(root, path))
                autostart_remove(vn);
            rc = hive_save(&h, root);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}

/* FILETIME of now, for the last-write time of enumerated keys */
static void write_filetime(FILETIME *ft)
{
    uint64_t t = aw_now_filetime();
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

/* copies `name` into a caller buffer of *cch characters (nul included) */
static LONG copy_name(const char *name, char *out, DWORD *cch)
{
    size_t n = strlen(name);
    if (*cch <= n) {
        *cch = (DWORD)n;
        return ERR_MORE_DATA;
    }
    memcpy(out, name, n + 1);
    *cch = (DWORD)n;
    return ERR_SUCCESS;
}

API(advapi32, LONG, RegEnumKeyExA, (HKEY hkey, DWORD index, LPSTR name, DWORD *cchName, DWORD *reserved, LPSTR cls,
                                   DWORD *cchCls, FILETIME *ft))
{
    (void)reserved;
    if (!name || !cchName)
        return ERR_INVALID_PARAMETER;
    char path[4096];
    reg_begin();
    int root = key_resolve(hkey, path, sizeof path);
    LONG rc = ERR_NO_MORE_ITEMS;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else {
        struct hive h;
        hive_load(&h, root);
        DWORD seen = 0;
        for (int i = 0; i < h.nkeys && rc == ERR_NO_MORE_ITEMS; i++) {
            if (!is_child_of(h.keys[i].path, path))
                continue;
            if (seen++ != index)
                continue;
            rc = copy_name(leaf_of(h.keys[i].path), name, cchName);
            if (cls && cchCls && *cchCls) {
                cls[0] = 0;
                *cchCls = 0;
            }
            if (ft)
                write_filetime(ft);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}

API(advapi32, LONG, RegEnumValueA, (HKEY hkey, DWORD index, LPSTR name, DWORD *cchName, DWORD *reserved, DWORD *type,
                                   BYTE *data, DWORD *cb))
{
    (void)reserved;
    if (!name || !cchName)
        return ERR_INVALID_PARAMETER;
    char path[4096];
    reg_begin();
    int root = key_resolve(hkey, path, sizeof path);
    LONG rc = ERR_NO_MORE_ITEMS;
    if (root < 0) {
        rc = ERR_INVALID_HANDLE;
    } else {
        struct hive h;
        hive_load(&h, root);
        DWORD seen = 0;
        for (int i = 0; i < h.nvals && rc == ERR_NO_MORE_ITEMS; i++) {
            const struct rval *v = &h.vals[i];
            if (strcasecmp(v->keypath, path) || seen++ != index)
                continue;
            rc = copy_name(v->name, name, cchName);
            if (rc != ERR_SUCCESS)
                break;
            DWORD outsz;
            uint8_t *out = from_stored(v->type, v->data, v->size, &outsz);
            if (type)
                *type = v->type;
            if (!data) {
                if (cb)
                    *cb = outsz;
            } else if (!cb) {
                rc = ERR_INVALID_PARAMETER;
            } else if (*cb < outsz) {
                *cb = outsz;
                rc = ERR_MORE_DATA;
            } else {
                memcpy(data, out, outsz);
                *cb = outsz;
            }
            free(out);
        }
        hive_free(&h);
    }
    reg_end();
    return rc;
}
