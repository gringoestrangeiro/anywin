/* String conversion and Windows <-> Unix path mapping */
#include "aw.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

size_t aw_wcslen(const WCHAR *s)
{
    size_t n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}

/* UTF-8 -> UTF-16. len = bytes or -1 (then nul is not counted).
 * Returns number of WCHARs needed/written (excluding nul). out may be NULL. */
int aw_u2w_buf(const char *s, int len, WCHAR *out, int outlen)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *e = len < 0 ? NULL : p + len;
    int n = 0;
    while (e ? p < e : *p) {
        const unsigned char *start = p;
        uint32_t c = *p++;
        if (c >= 0x80) {
            int extra = c >= 0xf8 ? -1 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : -1;
            if (extra < 0) {
                c = *start; /* invalid: treat the byte as Latin-1 */
            } else {
                c &= 0x3f >> extra;
                for (int i = 0; i < extra; i++) {
                    if ((e && p >= e) || (*p & 0xc0) != 0x80) {
                        c = *start;
                        p = start + 1;
                        goto put;
                    }
                    c = (c << 6) | (*p++ & 0x3f);
                }
            }
        }
    put:
        if (c >= 0x10000) {
            if (out && n + 1 < outlen) {
                c -= 0x10000;
                out[n] = 0xd800 | (c >> 10);
                out[n + 1] = 0xdc00 | (c & 0x3ff);
            }
            n += 2;
        } else {
            if (out && n < outlen)
                out[n] = (WCHAR)c;
            n++;
        }
    }
    return n;
}

int aw_w2u_buf(const WCHAR *w, int len, char *out, int outlen)
{
    int n = 0;
    for (int i = 0; len < 0 ? w[i] != 0 : i < len; i++) {
        uint32_t c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && (len < 0 || i + 1 < len) && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        }
        char b[4];
        int k;
        if (c < 0x80)
            b[0] = (char)c, k = 1;
        else if (c < 0x800)
            b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), k = 2;
        else if (c < 0x10000)
            b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)),
            k = 3;
        else
            b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3f)),
            b[2] = (char)(0x80 | ((c >> 6) & 0x3f)), b[3] = (char)(0x80 | (c & 0x3f)), k = 4;
        if (out) {
            if (n + k > outlen)
                return n + k; /* signal overflow by exceeding */
            memcpy(out + n, b, k);
        }
        n += k;
    }
    /* NUL-terminate whenever the buffer has room past the converted text (callers that
     * need a C string rely on it; the return value stays the byte count without the NUL) */
    if (out && n < outlen)
        out[n] = 0;
    return n;
}

/* Converts to UTF-8 for an A-API output buffer of outlen bytes: whole characters only, always
 * NUL-terminated. Returns the number of bytes stored before the NUL (GetWindowTextA semantics). */
int aw_w2u_term(const WCHAR *w, int len, char *out, int outlen)
{
    if (!out || outlen <= 0)
        return 0;
    int total = aw_w2u_buf(w, len, NULL, 0);
    char *cv = malloc((size_t)total + 1);
    if (!cv) {
        out[0] = 0;
        return 0;
    }
    aw_w2u_buf(w, len, cv, total);
    int m = total < outlen - 1 ? total : outlen - 1;
    while (m > 0 && m < total && ((unsigned char)cv[m] & 0xc0) == 0x80)
        m--;
    memcpy(out, cv, (size_t)m);
    out[m] = 0;
    free(cv);
    return m;
}

char *aw_w2u(const WCHAR *w, int len)
{
    if (!w)
        return NULL;
    int n = aw_w2u_buf(w, len, NULL, 0);
    char *s = malloc(n + 1);
    aw_w2u_buf(w, len, s, n);
    s[n] = 0;
    return s;
}

WCHAR *aw_u2w(const char *s, int len)
{
    if (!s)
        return NULL;
    int n = aw_u2w_buf(s, len, NULL, 0);
    WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
    aw_u2w_buf(s, len, w, n);
    w[n] = 0;
    return w;
}

/* ---- paths ---------------------------------------------------------------- */
static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static void drive_root(char letter, char *out, size_t n)
{
    letter = (char)tolower((unsigned char)letter);
    char var[32];
    snprintf(var, sizeof var, "ANYWIN_DRIVE_%c", toupper((unsigned char)letter));
    const char *v = getenv(var);
    if (v) {
        snprintf(out, n, "%s", v);
    } else if (letter == 'z') {
        out[0] = 0;
    } else {
        snprintf(out, n, "%s/.anywin/drive_%c", home_dir(), letter);
    }
    /* strip trailing slash */
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/')
        out[--l] = 0;
}

/* Resolve each path component case-insensitively if the exact one is absent. */
static void fix_case(char *path)
{
    struct stat st;
    if (!*path || lstat(path, &st) == 0)
        return;
    char *p = path;
    if (*p == '/')
        p++;
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash)
            *slash = 0;
        if (lstat(path, &st) != 0) {
            /* search the directory for a case-insensitive match */
            char saved = 0;
            char *dirend = p - 1;
            const char *dir;
            if (p == path)
                dir = ".";
            else if (dirend == path)
                dir = "/";
            else {
                saved = *dirend;
                *dirend = 0;
                dir = path;
            }
            DIR *d = opendir(dir);
            if (saved)
                *dirend = saved;
            int found = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)))
                    if (!strcasecmp(de->d_name, p) && strlen(de->d_name) == strlen(p)) {
                        memcpy(p, de->d_name, strlen(p));
                        found = 1;
                        break;
                    }
                closedir(d);
            }
            if (!found) {
                if (slash)
                    *slash = '/';
                return;
            }
        }
        if (!slash)
            break;
        *slash = '/';
        p = slash + 1;
    }
}

char *aw_unix_path(const char *w)
{
    if (!w)
        return NULL;
    char buf[4096];
    size_t n = 0;
    if (!strncmp(w, "\\\\?\\", 4) || !strncmp(w, "\\\\.\\", 4)) {
        w += 4;
        if (!strncasecmp(w, "UNC\\", 4))
            w += 3;
    }
    /* devices */
    static const struct { const char *win, *unix_; } dev[] = {
        {"NUL", "/dev/null"}, {"CON", "/dev/tty"}, {"CONOUT$", "/dev/stdout"}, {"CONIN$", "/dev/stdin"}};
    for (size_t i = 0; i < sizeof dev / sizeof *dev; i++)
        if (!strcasecmp(w, dev[i].win))
            return strdup(dev[i].unix_);

    if (((w[0] | 32) >= 'a' && (w[0] | 32) <= 'z') && w[1] == ':') {
        drive_root(w[0], buf, sizeof buf);
        n = strlen(buf);
        w += 2;
        if (*w != '\\' && *w != '/')
            buf[n++] = '/';
    }
    for (; *w && n < sizeof buf - 1; w++) {
        char c = *w == '\\' ? '/' : *w;
        if (c == '/' && n > 0 && buf[n - 1] == '/')
            continue;
        buf[n++] = c;
    }
    buf[n] = 0;
    if (!buf[0])
        strcpy(buf, "/");
    fix_case(buf);
    return strdup(buf);
}

char *aw_unix_pathW(const WCHAR *w)
{
    if (!w)
        return NULL;
    char *u = aw_w2u(w, -1);
    char *r = aw_unix_path(u);
    free(u);
    return r;
}

char *aw_win_path(const char *u)
{
    char buf[4096];
    size_t n = 0;
    if (u[0] == '/') {
        /* drive mappings other than Z: */
        for (char l = 'a'; l <= 'y'; l++) {
            char root[1024];
            drive_root(l, root, sizeof root);
            size_t rl = strlen(root);
            if (rl > 1 && !strncmp(u, root, rl) && (u[rl] == '/' || u[rl] == 0)) {
                snprintf(buf, sizeof buf, "%c:%s", toupper(l), u[rl] ? "" : "\\");
                n = strlen(buf);
                u += rl;
                goto copy;
            }
        }
        buf[0] = 'Z';
        buf[1] = ':';
        n = 2;
    }
copy:
    for (; *u && n < sizeof buf - 1; u++)
        buf[n++] = *u == '/' ? '\\' : *u;
    buf[n] = 0;
    return strdup(buf);
}

DWORD aw_errno_to_win(int e)
{
    switch (e) {
    case 0: return 0;
    case ENOENT: return 2;   /* ERROR_FILE_NOT_FOUND */
    case ENOTDIR: return 3;  /* ERROR_PATH_NOT_FOUND */
    case EMFILE: return 4;
    case EACCES: case EPERM: case EROFS: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EEXIST: return 183; /* ERROR_ALREADY_EXISTS */
    case ENOSPC: return 112;
    case ENOTEMPTY: return 145;
    case EINVAL: return 87;
    case EBUSY: return 32;   /* ERROR_SHARING_VIOLATION */
    case EPIPE: return 109;  /* ERROR_BROKEN_PIPE */
    case ENAMETOOLONG: return 206;
    case EXDEV: return 17;   /* ERROR_NOT_SAME_DEVICE */
    case EISDIR: return 5;
    case EAGAIN: return 997;
    default: return 31;      /* ERROR_GEN_FAILURE */
    }
}
