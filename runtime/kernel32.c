/* kernel32: process startup, modules, heap, virtual memory, resources,
 * time, version/system information, FormatMessage and misc exports. */
#include "aw.h"
#include "k32.h"
#include "modmap.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern const struct aw_export __start_aw_exports[], __stop_aw_exports[];

char aw_exe_unix[4096];
char aw_exe_win[4096];

static WCHAR *cmdline_w;
static char *cmdline_a;
static WCHAR *env_block_w;
static void *image_base_ptr;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static DWORD winerr(int e) { return aw_errno_to_win(e); }

static int has_exports(const char *module)
{
    for (const struct aw_export *e = __start_aw_exports; e < __stop_aw_exports; e++)
        if (!strcmp(e->module, module))
            return 1;
    return 0;
}

/* Windows command line quoting (MS CRT rules). */
static size_t quote_arg(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    int quote = !*s || strpbrk(s, " \t\n\v\"") != NULL;
#define PUT(c)                                                                                             \
    do {                                                                                                   \
        if (out && n < cap)                                                                                \
            out[n] = (c);                                                                                  \
        n++;                                                                                               \
    } while (0)
    if (!quote) {
        while (*s)
            PUT(*s++);
        return n;
    }
    PUT('"');
    for (;;) {
        size_t bs = 0;
        while (*s == '\\') {
            s++;
            bs++;
        }
        if (!*s) {
            for (size_t i = 0; i < bs * 2; i++)
                PUT('\\');
            break;
        }
        if (*s == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++)
                PUT('\\');
            PUT('"');
            s++;
            continue;
        }
        for (size_t i = 0; i < bs; i++)
            PUT('\\');
        PUT(*s++);
    }
    PUT('"');
#undef PUT
    return n;
}

static void build_command_line(int argc, char **argv)
{
    size_t len = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = i == 0 ? aw_exe_win : argv[i];
        if (i)
            len++;
        len += quote_arg(NULL, 0, a);
    }
    char *a = malloc(len + 1);
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            a[o++] = ' ';
        o += quote_arg(a + o, len + 1 - o, i == 0 ? aw_exe_win : argv[i]);
    }
    a[o] = 0;
    cmdline_a = a;
    cmdline_w = aw_u2w(a, -1);
}

/* environment ------------------------------------------------------- */
static char *env_name_of(const char *entry)
{
    const char *eq = strchr(entry, '=');
    if (!eq)
        return NULL;
    size_t n = eq - entry;
    char *s = malloc(n + 1);
    memcpy(s, entry, n);
    s[n] = 0;
    return s;
}

static char *env_find(const char *name, char **out_entry)
{
    size_t nl = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (!strncasecmp(*e, name, nl) && (*e)[nl] == '=') {
            if (out_entry)
                *out_entry = *e;
            return *e + nl + 1;
        }
    }
    return NULL;
}

static void env_default(const char *name, const char *value)
{
    if (!env_find(name, NULL))
        setenv(name, value, 0);
}

static void env_default_path(const char *name, const char *unixdir)
{
    if (env_find(name, NULL))
        return;
    char *w = aw_win_path(unixdir);
    setenv(name, w, 0);
    free(w);
}

void aw_init_environment(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    char buf[4096];
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *data = getenv("XDG_DATA_HOME");
    if (cfg && *cfg == '/')
        env_default_path("APPDATA", cfg);
    else {
        snprintf(buf, sizeof buf, "%s/.config", home);
        env_default_path("APPDATA", buf);
    }
    if (data && *data == '/')
        env_default_path("LOCALAPPDATA", data);
    else {
        snprintf(buf, sizeof buf, "%s/.local/share", home);
        env_default_path("LOCALAPPDATA", buf);
    }
    char *up = aw_win_path(home);
    setenv("USERPROFILE", up, 0);
    if (strlen(up) >= 2 && up[1] == ':') {
        char drive[3] = {up[0], ':', 0};
        setenv("HOMEDRIVE", drive, 0);
        setenv("HOMEPATH", up + 2, 0);
    }
    free(up);
    env_default("TEMP", "Z:\\tmp");
    env_default("TMP", "Z:\\tmp");
    const char *user = getenv("USER");
    if (!user || !*user)
        user = getenv("LOGNAME");
    if (!user || !*user)
        user = "user";
    env_default("USERNAME", user);
    char host[256] = "anywin";
    if (!gethostname(host, sizeof host - 1)) {
        host[sizeof host - 1] = 0;
        for (char *p = host; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        char *dot = strchr(host, '.');
        if (dot)
            *dot = 0;
    } else
        strcpy(host, "ANYWIN");
    env_default("COMPUTERNAME", host);
    env_default("SystemRoot", "C:\\windows");
    env_default("windir", "C:\\windows");
    env_default("SystemDrive", "C:");
    env_default("ProgramFiles", "C:\\Program Files");
    env_default("ProgramFiles(x86)", "C:\\Program Files (x86)");
    env_default("ProgramData", "C:\\ProgramData");
    env_default("ALLUSERSPROFILE", "C:\\ProgramData");
    env_default("PUBLIC", "C:\\Users\\Public");
    env_default("OS", "Windows_NT");
    env_default("PROCESSOR_ARCHITECTURE", "AMD64");
    env_default("PROCESSOR_IDENTIFIER", "AMD64 Family 6 Model 60 Stepping 3, GenuineIntel");
    env_default("PROCESSOR_LEVEL", "6");
    env_default("PROCESSOR_REVISION", "3c03");
    snprintf(buf, sizeof buf, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    env_default("NUMBER_OF_PROCESSORS", buf);
    env_default("PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    env_default("ComSpec", "C:\\windows\\system32\\cmd.exe");
    env_default_path("PROGRAMDATA", "/var/lib");
}

/* Build a Windows environment block (UTF-16, double nul terminated). */
static WCHAR *make_env_block(void)
{
    size_t count = 0;
    for (char **e = environ; e && *e; e++)
        count++;
    char **list = malloc((count + 1) * sizeof(char *));
    size_t n = 0;
    for (char **e = environ; e && *e; e++)
        if (strchr(*e, '=') && (*e)[0] != '=')
            list[n++] = *e;
    int cmp(const void *a, const void *b);
    qsort(list, n, sizeof(char *), (int (*)(const void *, const void *))strcasecmp);
    size_t wlen = 0;
    for (size_t i = 0; i < n; i++) {
        WCHAR *w = aw_u2w(list[i], -1);
        wlen += aw_wcslen(w) + 1;
        free(w);
    }
    WCHAR *block = calloc(wlen + 1, sizeof(WCHAR));
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        WCHAR *w = aw_u2w(list[i], -1);
        size_t l = aw_wcslen(w);
        memcpy(block + o, w, l * sizeof(WCHAR));
        o += l + 1;
        free(w);
    }
    block[o] = 0;
    free(list);
    return block;
}

static char *make_env_block_a(void)
{
    size_t total = 0;
    for (char **e = environ; e && *e; e++)
        if ((*e)[0] != '=')
            total += strlen(*e) + 1;
    char *b = malloc(total + 1);
    size_t o = 0;
    for (char **e = environ; e && *e; e++)
        if ((*e)[0] != '=') {
            size_t l = strlen(*e) + 1;
            memcpy(b + o, *e, l);
            o += l;
        }
    b[o] = 0;
    return b;
}

/* PEB / process parameters ------------------------------------------ */
static uint8_t *peb_addr(void)
{
    void *p;
    memcpy(&p, (uint8_t *)aw_teb_current() + 0x60, sizeof p);
    return p;
}

typedef struct {
    uint16_t Length, MaxLength;
    uint32_t pad;
    WCHAR *Buffer;
} USTRING;

static const WCHAR empty_w[1];

static void ustring_set(USTRING *u, const WCHAR *s, int maxchars)
{
    size_t l = s ? aw_wcslen(s) : 0;
    u->Buffer = (WCHAR *)s;
    u->Length = (uint16_t)(l * sizeof(WCHAR));
    u->MaxLength = (uint16_t)(maxchars * sizeof(WCHAR));
}

/* standard handles are owned by file.c */
HANDLE aw_std_handle(int which)
{
    static HANDLE dummy[3];
    extern HANDLE *aw_std_handle_slot(int which);
    HANDLE *s = aw_std_handle_slot(which);
    return s ? *s : dummy[which];
}

void *aw_get_image_base(void) { return __aw_image_base; }

/* ------------------------------------------------------------------ */
/* exports: process / command line / startup info                       */
/* ------------------------------------------------------------------ */
API(kernel32, HANDLE, GetCurrentProcess, (void)) { return (HANDLE)(intptr_t)-1; }
API(kernel32, DWORD, GetCurrentProcessId, (void)) { return (DWORD)getpid(); }
API(kernel32, void, ExitProcess, (UINT code))
{
    if (aw_gui_atexit)
        aw_gui_atexit();
    fflush(NULL);
    _exit((int)code);
}
API(kernel32, BOOL, TerminateProcess, (HANDLE h, UINT code))
{
    if ((intptr_t)h == -1) {
        api_exit:
        if (aw_gui_atexit)
            aw_gui_atexit();
        fflush(NULL);
        _exit((int)code);
    }
    extern DWORD aw_process_id(HANDLE);
    DWORD pid = aw_process_id(h);
    if (pid && pid != (DWORD)getpid()) {
        kill(pid, SIGKILL);
        extern void aw_process_mark_dead(HANDLE, DWORD);
        aw_process_mark_dead(h, code);
        return TRUE;
    }
    if (pid == (DWORD)getpid())
        goto api_exit;
    aw_set_last_error(6);
    return FALSE;
}

API(kernel32, LPSTR, GetCommandLineA, (void)) { return cmdline_a; }
API(kernel32, LPWSTR, GetCommandLineW, (void)) { return cmdline_w; }

/* STARTUPINFOA / W layout is identical except the string pointer width. */
typedef struct {
    DWORD cb;
    void *lpReserved, *lpDesktop, *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    void *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOX;
_Static_assert(sizeof(STARTUPINFOX) == 104, "STARTUPINFO size");

API(kernel32, void, GetStartupInfoA, (STARTUPINFOX * si))
{
    if (!si)
        return;
    memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
    si->wShowWindow = 10; /* SW_SHOWDEFAULT */
    si->hStdInput = aw_std_handle(0);
    si->hStdOutput = aw_std_handle(1);
    si->hStdError = aw_std_handle(2);
}
API(kernel32, void, GetStartupInfoW, (STARTUPINFOX * si))
{
    __aw_kernel32_GetStartupInfoA(si);
}

static void ensure_env_block(void)
{
    if (!env_block_w)
        env_block_w = make_env_block();
}

API(kernel32, DWORD, GetEnvironmentVariableA, (const char *name, char *buf, DWORD n))
{
    if (!name || !*name) {
        aw_set_last_error(87);
        return 0;
    }
    char *v = env_find(name, NULL);
    if (!v) {
        aw_set_last_error(203);
        return 0;
    }
    DWORD need = (DWORD)strlen(v) + 1;
    if (n < need) {
        if (buf && n)
            buf[0] = 0;
        return need;
    }
    strcpy(buf, v);
    return need - 1;
}
API(kernel32, DWORD, GetEnvironmentVariableW, (const WCHAR *name, WCHAR *buf, DWORD n))
{
    char *nm = aw_w2u(name, -1);
    char *v = nm ? env_find(nm, NULL) : NULL;
    free(nm);
    if (!v) {
        aw_set_last_error(203);
        return 0;
    }
    WCHAR *w = aw_u2w(v, -1);
    DWORD need = (DWORD)aw_wcslen(w) + 1;
    if (n < need) {
        if (buf && n)
            buf[0] = 0;
        free(w);
        return need;
    }
    memcpy(buf, w, need * sizeof(WCHAR));
    free(w);
    return need - 1;
}

API(kernel32, BOOL, SetEnvironmentVariableA, (const char *name, const char *value))
{
    if (!name || !*name || strchr(name, '=') || !strncmp(name, "=", 1)) {
        aw_set_last_error(87);
        return FALSE;
    }
    char *entry = NULL;
    env_find(name, &entry);
    if (!value) {
        if (entry) {
            char *nm = env_name_of(entry);
            if (nm) {
                unsetenv(nm);
                free(nm);
            }
        }
        env_block_w = NULL;
        return TRUE;
    }
    if (entry) {
        char *nm = env_name_of(entry);
        if (nm) {
            setenv(nm, value, 1);
            free(nm);
        }
    } else
        setenv(name, value, 1);
    env_block_w = NULL;
    return TRUE;
}
API(kernel32, BOOL, SetEnvironmentVariableW, (const WCHAR *name, const WCHAR *value))
{
    char *n = aw_w2u(name, -1);
    char *v = value ? aw_w2u(value, -1) : NULL;
    BOOL r = __aw_kernel32_SetEnvironmentVariableA(n, v);
    free(n);
    free(v);
    return r;
}

API(kernel32, LPSTR, GetEnvironmentStringsA, (void)) { return make_env_block_a(); }
API(kernel32, LPWSTR, GetEnvironmentStringsW, (void))
{
    ensure_env_block();
    return env_block_w;
}
API(kernel32, LPWSTR, GetEnvironmentStrings, (void)) { return __aw_kernel32_GetEnvironmentStringsW(); }
API(kernel32, BOOL, FreeEnvironmentStringsA, (char *p)) { free(p); return TRUE; }
API(kernel32, BOOL, FreeEnvironmentStringsW, (WCHAR *p))
{
    if (p && p == env_block_w)
        return TRUE; /* the PEB owns this block */
    free(p);
    return TRUE;
}
API(kernel32, BOOL, FreeEnvironmentStrings, (WCHAR *p)) { return __aw_kernel32_FreeEnvironmentStringsW(p); }

/* Expand %VAR% references.  Returns required length including the nul. */
static int expand_env_w(const WCHAR *src, WCHAR *dst, int n)
{
    int o = 0;
    for (const WCHAR *p = src; *p;) {
        if (*p != '%') {
            if (dst && o + 1 < n)
                dst[o] = *p;
            o++;
            p++;
            continue;
        }
        const WCHAR *e = p + 1;
        while (*e && *e != '%')
            e++;
        if (*e != '%' || e == p + 1) {
            if (dst && o + 1 < n)
                dst[o] = '%';
            o++;
            p++;
            continue;
        }
        char *nm = aw_w2u(p + 1, (int)(e - p - 1));
        char *v = nm ? env_find(nm, NULL) : NULL;
        free(nm);
        if (!v) {
            for (const WCHAR *q = p; q <= e; q++) {
                if (dst && o + 1 < n)
                    dst[o] = *q;
                o++;
            }
        } else {
            WCHAR *w = aw_u2w(v, -1);
            for (const WCHAR *q = w; *q; q++) {
                if (dst && o + 1 < n)
                    dst[o] = *q;
                o++;
            }
            free(w);
        }
        p = e + 1;
    }
    if (dst && o < n)
        dst[o] = 0;
    return o + 1;
}
API(kernel32, DWORD, ExpandEnvironmentStringsW, (const WCHAR *src, WCHAR *dst, DWORD n))
{
    return (DWORD)expand_env_w(src, dst, (int)n);
}
API(kernel32, DWORD, ExpandEnvironmentStringsA, (const char *src, char *dst, DWORD n))
{
    WCHAR *w = aw_u2w(src, -1);
    int need = expand_env_w(w, NULL, 0);
    WCHAR *o = malloc((size_t)need * sizeof(WCHAR));
    expand_env_w(w, o, need);
    char *u = aw_w2u(o, -1);
    DWORD r = (DWORD)strlen(u) + 1;
    if (n >= r && dst)
        memcpy(dst, u, r);
    free(w);
    free(o);
    free(u);
    return r;
}

/* ------------------------------------------------------------------ */
/* modules                                                             */
/* ------------------------------------------------------------------ */
#define FAKE_MAGIC 0x46414b45u
typedef struct aw_fake_module {
    uint8_t hdr[0x400];
    uint32_t magic;
    int refs;
    char name[64];
    struct aw_fake_module *next;
} aw_fake_module;

static aw_fake_module *fakes;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static void fake_fill(aw_fake_module *fm, const char *name)
{
    uint8_t *h = fm->hdr;
    memset(h, 0, sizeof fm->hdr);
    h[0] = 'M';
    h[1] = 'Z';
    *(uint32_t *)(h + 0x3c) = 0x80;
    memcpy(h + 0x80, "PE\0\0", 4);
    uint8_t *coff = h + 0x84;
    *(uint16_t *)(coff) = 0x8664;
    *(uint16_t *)(coff + 16) = 0xf0;
    *(uint16_t *)(coff + 18) = 0x2022;
    uint8_t *opt = coff + 20;
    *(uint16_t *)opt = 0x20b;
    *(uint8_t *)(opt + 2) = 14;
    *(uint64_t *)(opt + 24) = (uint64_t)(uintptr_t)fm;
    *(uint32_t *)(opt + 32) = 0x1000;
    *(uint32_t *)(opt + 36) = 0x200;
    *(uint32_t *)(opt + 56) = 0x1000;
    *(uint32_t *)(opt + 60) = 0x200;
    *(uint16_t *)(opt + 68) = 3;
    *(uint32_t *)(opt + 108) = 16;
    snprintf(fm->name, sizeof fm->name, "%s", name);
}

static aw_fake_module *fake_get(const char *name, int create)
{
    pthread_mutex_lock(&fake_lock);
    for (aw_fake_module *fm = fakes; fm; fm = fm->next)
        if (!strcmp(fm->name, name)) {
            pthread_mutex_unlock(&fake_lock);
            return fm;
        }
    aw_fake_module *fm = NULL;
    if (create && has_exports(name)) {
        fm = calloc(1, sizeof *fm);
        fake_fill(fm, name);
        fm->magic = FAKE_MAGIC;
        fm->refs = 1;
        fm->next = fakes;
        fakes = fm;
    }
    pthread_mutex_unlock(&fake_lock);
    return fm;
}

static aw_fake_module *fake_from_handle(HMODULE h)
{
    if (!h)
        return NULL;
    pthread_mutex_lock(&fake_lock);
    for (aw_fake_module *fm = fakes; fm; fm = fm->next)
        if ((HMODULE)fm == h && fm->magic == FAKE_MAGIC) {
            pthread_mutex_unlock(&fake_lock);
            return fm;
        }
    pthread_mutex_unlock(&fake_lock);
    return NULL;
}

static const char *own_basename(void)
{
    const char *s = strrchr(aw_exe_win, '\\');
    return s ? s + 1 : aw_exe_win;
}

static int is_own_name(const char *name)
{
    if (!name || !*name)
        return 1;
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    if (!strcasecmp(base, own_basename()))
        return 1;
    size_t l = strlen(base);
    if (l > 4 && !strcasecmp(base + l - 4, ".exe") && strlen(own_basename()) > 4 &&
        !strncasecmp(base, own_basename(), l - 4) && strlen(own_basename()) == l)
        return 1;
    return 0;
}

static HMODULE module_handle(const char *name)
{
    if (is_own_name(name))
        return (HMODULE)__aw_image_base;
    char norm[256];
    snprintf(norm, sizeof norm, "%s", aw_normalize_module(name));
    aw_fake_module *fm = fake_get(norm, 1);
    if (!fm) {
        aw_set_last_error(126);
        return NULL;
    }
    return (HMODULE)fm;
}

API(kernel32, HMODULE, GetModuleHandleA, (const char *name))
{
    if (name) {
        char *n = name[0] ? strdup(name) : NULL;
        HMODULE h = module_handle(n);
        free(n);
        return h;
    }
    return (HMODULE)__aw_image_base;
}
API(kernel32, HMODULE, GetModuleHandleW, (const WCHAR *name))
{
    if (!name)
        return (HMODULE)__aw_image_base;
    char *n = aw_w2u(name, -1);
    HMODULE h = module_handle(n);
    free(n);
    return h;
}
API(kernel32, BOOL, GetModuleHandleExA, (DWORD flags, const char *name, HMODULE *out))
{
    HMODULE h = NULL;
    if (flags & 4) { /* FROM_ADDRESS */
        uintptr_t a = (uintptr_t)name;
        uint8_t *base = (uint8_t *)__aw_image_base;
        extern const char __aw_image_size[];
        if (a >= (uintptr_t)base && a < (uintptr_t)base + (uintptr_t)__aw_image_size)
            h = (HMODULE)base;
        else
            for (const struct aw_export *e = __start_aw_exports; e < __stop_aw_exports; e++)
                if ((uintptr_t)e->addr == a) {
                    h = (HMODULE)fake_get(e->module, 1);
                    break;
                }
        if (!h) {
            aw_set_last_error(126);
            return FALSE;
        }
    } else
        h = module_handle(name);
    if (!h)
        return FALSE;
    if (out)
        *out = h;
    return TRUE;
}
API(kernel32, BOOL, GetModuleHandleExW, (DWORD flags, const WCHAR *name, HMODULE *out))
{
    char *n = name && !(flags & 4) ? aw_w2u(name, -1) : NULL;
    BOOL ok = __aw_kernel32_GetModuleHandleExA(flags, (const char *)((flags & 4) ? (const void *)name : n), out);
    free(n);
    return ok;
}

API(kernel32, HMODULE, LoadLibraryA, (const char *name))
{
    if (!name || !*name) {
        aw_set_last_error(87);
        return NULL;
    }
    return module_handle(name);
}
API(kernel32, HMODULE, LoadLibraryW, (const WCHAR *name))
{
    char *n = aw_w2u(name, -1);
    HMODULE h = __aw_kernel32_LoadLibraryA(n);
    free(n);
    return h;
}
API(kernel32, HMODULE, LoadLibraryExA, (const char *name, HANDLE file, DWORD flags))
{
    (void)file;
    (void)flags;
    return __aw_kernel32_LoadLibraryA(name);
}
API(kernel32, HMODULE, LoadLibraryExW, (const WCHAR *name, HANDLE file, DWORD flags))
{
    (void)file;
    (void)flags;
    return __aw_kernel32_LoadLibraryW(name);
}
API(kernel32, BOOL, FreeLibrary, (HMODULE h))
{
    if ((void *)h == __aw_image_base)
        return TRUE;
    aw_fake_module *fm = fake_from_handle(h);
    if (!fm) {
        aw_set_last_error(126);
        return FALSE;
    }
    if (fm->refs > 0)
        fm->refs--;
    return TRUE;
}
API(kernel32, BOOL, DisableThreadLibraryCalls, (HMODULE h))
{
    (void)h;
    return TRUE;
}

/* own image export directory */
static void *image_export(const char *name, int ord, int by_ord)
{
    uint8_t *base = (uint8_t *)__aw_image_base;
    uint32_t pe = *(uint32_t *)(base + 0x3c);
    uint8_t *opt = base + pe + 24;
    uint32_t ndirs = *(uint32_t *)(opt + 108);
    if (ndirs < 1)
        return NULL;
    uint32_t erva = *(uint32_t *)(opt + 112);
    if (!erva)
        return NULL;
    uint8_t *ed = base + erva;
    uint32_t nfuncs = *(uint32_t *)(ed + 20);
    uint32_t nnames = *(uint32_t *)(ed + 24);
    uint32_t afuncs = *(uint32_t *)(ed + 28);
    uint32_t anames = *(uint32_t *)(ed + 32);
    uint32_t aords = *(uint32_t *)(ed + 36);
    uint32_t obase = *(uint32_t *)(ed + 16);
    if (by_ord) {
        uint32_t idx = (uint32_t)ord - obase;
        if (idx >= nfuncs)
            return NULL;
        uint32_t rva = *(uint32_t *)(base + afuncs + idx * 4);
        if (!rva)
            return NULL;
        if (rva >= erva && rva < erva + *(uint32_t *)(ed + 4))
            return NULL; /* forwarder: unsupported */
        return base + rva;
    }
    for (uint32_t i = 0; i < nnames; i++) {
        uint32_t nrva = *(uint32_t *)(base + anames + i * 4);
        const char *n = (const char *)(base + nrva);
        if (!strcmp(n, name)) {
            uint16_t oi = *(uint16_t *)(base + aords + i * 2);
            if (oi >= nfuncs)
                return NULL;
            uint32_t rva = *(uint32_t *)(base + afuncs + oi * 4);
            if (!rva)
                return NULL;
            if (rva >= erva && rva < erva + *(uint32_t *)(ed + 4))
                return NULL;
            return base + rva;
        }
    }
    return NULL;
}

API(kernel32, void *, GetProcAddress, (HMODULE h, const char *name))
{
    uintptr_t v = (uintptr_t)name;
    int by_ord = IS_INTRESOURCE(name) != 0;
    int ord = (int)(v & 0xffff);
    if (!h) {
        aw_set_last_error(126);
        return NULL;
    }
    void *r = NULL;
    if ((void *)h == __aw_image_base) {
        r = image_export(name, ord, by_ord);
        if (!r && by_ord) {
            char fn[32];
            snprintf(fn, sizeof fn, "#%d", ord);
            r = image_export(fn, 0, 0);
        }
    } else if (fake_from_handle(h)) {
        aw_fake_module *fm = (aw_fake_module *)h;
        char fn[128];
        if (by_ord) {
            const char *n = aw_ordinal_name(fm->name, ord);
            if (n)
                snprintf(fn, sizeof fn, "%s", n);
            else
                snprintf(fn, sizeof fn, "#%d", ord);
            r = aw_find_export(fm->name, fn);
        } else
            r = aw_find_export(fm->name, name);
    }
    if (!r)
        aw_set_last_error(127);
    return r;
}

static DWORD module_name(HMODULE h, char *out, DWORD n)
{
    char path[4096];
    if ((void *)h == __aw_image_base || !h) {
        snprintf(path, sizeof path, "%s", aw_exe_win);
    } else {
        aw_fake_module *fm = fake_from_handle(h);
        if (!fm) {
            aw_set_last_error(6);
            return 0;
        }
        snprintf(path, sizeof path, "C:\\windows\\system32\\%s.dll", fm->name);
    }
    DWORD need = (DWORD)strlen(path) + 1;
    if (n < need) {
        if (out && n)
            out[0] = 0;
        aw_set_last_error(122);
        return n;
    }
    memcpy(out, path, need);
    return need - 1;
}

API(kernel32, DWORD, GetModuleFileNameA, (HMODULE h, char *buf, DWORD n))
{
    return module_name(h, buf, n);
}
API(kernel32, DWORD, GetModuleFileNameW, (HMODULE h, WCHAR *buf, DWORD n))
{
    char a[4096];
    DWORD r = module_name(h, a, sizeof a);
    if (!r)
        return 0;
    WCHAR *w = aw_u2w(a, -1);
    DWORD need = (DWORD)aw_wcslen(w) + 1;
    if (n < need) {
        if (n)
            buf[0] = 0;
        free(w);
        aw_set_last_error(122);
        return n;
    }
    memcpy(buf, w, need * sizeof(WCHAR));
    free(w);
    return need - 1;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */
void aw_kernel32_init(int argc, char **argv, char **envp)
{
    (void)envp;
    ssize_t l = readlink("/proc/self/exe", aw_exe_unix, sizeof aw_exe_unix - 1);
    if (l < 0)
        l = 0;
    aw_exe_unix[l] = 0;
    char *w = aw_win_path(aw_exe_unix);
    snprintf(aw_exe_win, sizeof aw_exe_win, "%s", w);
    free(w);
    image_base_ptr = __aw_image_base;

    aw_file_init();
    aw_console_init();
    aw_init_environment();
    build_command_line(argc, argv);
    aw_std_init();

    ensure_env_block();
    uint8_t *peb = peb_addr();
    uint8_t *pp = NULL;
    memcpy(&pp, peb + 0x20, sizeof pp);
    if (pp) {
        *(uint32_t *)(pp + 0x00) = 0x400;
        *(uint32_t *)(pp + 0x04) = 0x100;
        *(uint32_t *)(pp + 0x08) = 0;
        void *h = aw_std_handle(0);
        memcpy(pp + 0x10, &h, 8);
        *(uint32_t *)(pp + 0x18) = 1; /* CONSOLE_HANDLE_VALID? (informational) */
        h = aw_std_handle(0);
        memcpy(pp + 0x20, &h, 8);
        h = aw_std_handle(1);
        memcpy(pp + 0x28, &h, 8);
        h = aw_std_handle(2);
        memcpy(pp + 0x30, &h, 8);
        WCHAR *cw = aw_u2w(aw_curdir_win, -1);
        ustring_set((USTRING *)(pp + 0x38), cw, MAX_PATH);
        *(uint64_t *)(pp + 0x48) = 0;
        ustring_set((USTRING *)(pp + 0x50), empty_w, 0);
        WCHAR *iw = aw_u2w(aw_exe_win, -1);
        ustring_set((USTRING *)(pp + 0x60), iw, 0x1000);
        ustring_set((USTRING *)(pp + 0x70), cmdline_w, 0x8000);
        *(void **)(pp + 0x80) = env_block_w;
        *(uint32_t *)(pp + 0xa0) = 10; /* SW_SHOWDEFAULT */
    }
    /* PEB.ProcessHeap (offset 0x30) */
    extern HANDLE __aw_kernel32_GetProcessHeap(void);
    void *h = __aw_kernel32_GetProcessHeap();
    memcpy(peb + 0x30, &h, sizeof h);
    TRACE("kernel32 init: exe=%s cmdline=%s", aw_exe_win, cmdline_a);
}
