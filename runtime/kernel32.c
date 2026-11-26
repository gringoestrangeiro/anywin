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
#include <strings.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern const struct aw_export __start_aw_exports[], __stop_aw_exports[];

char aw_exe_unix[4096];
char aw_exe_win[4096];
char aw_curdir_win[4096];

static WCHAR *cmdline_w;
static char *cmdline_a;
static WCHAR *env_block_w;
static HANDLE std_handles[3];
static void *unhandled_filter;
static char process_heap_marker[16]; /* identity of the process heap handle */

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
/* the argument is always evaluated: the sizing pass (out == NULL) must advance s too */
#define PUT(c)                                                                                             \
    do {                                                                                                   \
        char put_c_ = (c);                                                                                 \
        if (out && n < cap)                                                                                \
            out[n] = put_c_;                                                                               \
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
}

static int cmp_env_entry(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
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
    qsort(list, n, sizeof(char *), cmp_env_entry);
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

HANDLE aw_std_handle(int which)
{
    return which >= 0 && which < 3 ? std_handles[which] : NULL;
}

void aw_set_std_handle(int which, HANDLE h)
{
    if (which >= 0 && which < 3)
        std_handles[which] = h;
}

void *aw_process_heap(void) { return process_heap_marker; }
void *aw_unhandled_filter(void) { return unhandled_filter; }

/* stdin/stdout/stderr as H_FILE handles on fds 0..2 (console flag when a tty) */
static void std_handles_init(void)
{
    for (int fd = 0; fd < 3; fd++) {
        aw_object *o = aw_object_new(H_FILE);
        aw_obj_set_fd(o, fd);
        *aw_obj_console(o) = isatty(fd) ? 1 : 0;
        *aw_obj_inherit(o) = 1;
        std_handles[fd] = aw_handle_alloc(H_FILE, o);
    }
}

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
    /* Only the current process can be named: CreateProcess is not in Stage 1, so there are no
     * child processes and no real process handles to terminate. */
    if ((intptr_t)h == -1)
        __aw_kernel32_ExitProcess(code);
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
API(kernel32, LPSTR, GetEnvironmentStrings, (void)) { return __aw_kernel32_GetEnvironmentStringsA(); }
API(kernel32, BOOL, FreeEnvironmentStringsA, (char *p)) { free(p); return TRUE; }
API(kernel32, BOOL, FreeEnvironmentStringsW, (WCHAR *p))
{
    if (p && p == env_block_w)
        return TRUE; /* the PEB owns this block */
    free(p);
    return TRUE;
}
API(kernel32, BOOL, FreeEnvironmentStrings, (char *p)) { return __aw_kernel32_FreeEnvironmentStringsA(p); }

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
    if (n < need) { /* Windows copies the truncated name and returns nSize */
        if (out && n) {
            memcpy(out, path, n - 1);
            out[n - 1] = 0;
        }
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
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
        w = aw_win_path(cwd);
        snprintf(aw_curdir_win, sizeof aw_curdir_win, "%s", w);
        free(w);
    } else
        strcpy(aw_curdir_win, "Z:\\");

    std_handles_init();
    aw_init_environment();
    build_command_line(argc, argv);

    ensure_env_block();
    uint8_t *peb = peb_addr();
    uint8_t *pp = NULL;
    memcpy(&pp, peb + 0x20, sizeof pp);
    if (pp) {
        /* RTL_USER_PROCESS_PARAMETERS, x64 layout */
        *(uint32_t *)(pp + 0x00) = 0x400;
        *(uint32_t *)(pp + 0x04) = 0x400;
        *(uint32_t *)(pp + 0x08) = 0;
        *(uint64_t *)(pp + 0x10) = 0; /* ConsoleHandle: none */
        *(uint32_t *)(pp + 0x18) = 0; /* ConsoleFlags */
        void *h = aw_std_handle(0);
        memcpy(pp + 0x20, &h, 8);
        h = aw_std_handle(1);
        memcpy(pp + 0x28, &h, 8);
        h = aw_std_handle(2);
        memcpy(pp + 0x30, &h, 8);
        /* CURDIR at 0x38 (DosPath UNICODE_STRING + Handle), DllPath at 0x50 */
        WCHAR *cw = aw_u2w(aw_curdir_win, -1);
        ustring_set((USTRING *)(pp + 0x38), cw, MAX_PATH);
        *(uint64_t *)(pp + 0x48) = 0;
        ustring_set((USTRING *)(pp + 0x50), empty_w, 0);
        WCHAR *iw = aw_u2w(aw_exe_win, -1);
        ustring_set((USTRING *)(pp + 0x60), iw, 0x1000);
        ustring_set((USTRING *)(pp + 0x70), cmdline_w, 0x7fff);
        *(void **)(pp + 0x80) = env_block_w;
        *(uint32_t *)(pp + 0xa8) = 10; /* ShowWindowFlags = SW_SHOWDEFAULT */
    }
    void *heap = aw_process_heap();
    memcpy(peb + 0x30, &heap, sizeof heap); /* PEB.ProcessHeap */
    TRACE("kernel32 init: exe=%s cmdline=%s", aw_exe_win, cmdline_a);
}

/* ------------------------------------------------------------------ */
/* last error, time, tick counts                                        */
/* ------------------------------------------------------------------ */
API(kernel32, DWORD, GetLastError, (void)) { return aw_get_last_error(); }
API(kernel32, void, SetLastError, (DWORD e)) { aw_set_last_error(e); }

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
uint32_t aw_tick_ms(void) { return (uint32_t)mono_ms(); }
API(kernel32, DWORD, GetTickCount, (void)) { return aw_tick_ms(); }
API(kernel32, ULONGLONG, GetTickCount64, (void)) { return mono_ms(); }

/* 100-ns intervals since 1601-01-01 UTC */
uint64_t aw_now_filetime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
}

/* ------------------------------------------------------------------ */
/* handles: std handles, synchronous file writes, handle release hooks  */
/* ------------------------------------------------------------------ */
API(kernel32, HANDLE, GetStdHandle, (DWORD which))
{
    switch (which) {
    case (DWORD)-10: return std_handles[0];
    case (DWORD)-11: return std_handles[1];
    case (DWORD)-12: return std_handles[2];
    }
    aw_set_last_error(87);
    return INVALID_HANDLE_VALUE;
}

API(kernel32, BOOL, WriteFile, (HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov))
{
    /* no overlapped I/O: writes complete before returning, so ov only receives the final result */
    if (written)
        *written = 0;
    int fd = aw_handle_fd(h);
    if (fd < 0) {
        aw_set_last_error(6);
        return FALSE;
    }
    const uint8_t *p = buf;
    DWORD done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            aw_set_last_error(winerr(errno));
            break;
        }
        done += (DWORD)w;
    }
    if (written)
        *written = done;
    if (ov) {
        *(uint64_t *)((char *)ov + 0) = done == n ? 0 : 5; /* STATUS_ACCESS_DENIED for a short write */
        *(uint64_t *)((char *)ov + 8) = done;
    }
    return done == n;
}

/* sync.c hooks: CloseHandle of H_FILE / H_FIND / H_MAPPING, H_PROCESS, and waits */
void aw_file_close(aw_object *o)
{
    int fd = aw_obj_fd(o);
    char **path = aw_obj_path(o);
    if (aw_obj_type(o) == H_FIND) {
        aw_find_release(*aw_obj_ext(o)); /* file.c: DIR and pattern of a FindFirstFile handle */
        *aw_obj_ext(o) = NULL;
    }
    if (fd >= 0) {
        close(fd);
        aw_obj_set_fd(o, -1);
    }
    if (*path && *aw_obj_delete_on_close(o))
        unlink(*path);
    free(*path);
    *path = NULL;
}

/* process objects (k32_proc.c): CloseHandle and child reaping */

/* QueueUserAPC is not in Stage 1, so no APC can ever be pending */
int aw_apc_run(DWORD tid)
{
    (void)tid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* files and directories (Tier 1: CreateDirectoryA only)                */
/* ------------------------------------------------------------------ */
API(kernel32, BOOL, CreateDirectoryA, (const char *path, void *sa))
{
    (void)sa;
    if (!path || !*path) {
        aw_set_last_error(3); /* ERROR_PATH_NOT_FOUND */
        return FALSE;
    }
    char *u = aw_unix_path(path);
    int r = mkdir(u, 0777);
    int e = errno;
    free(u);
    if (r) {
        aw_set_last_error(winerr(e));
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* arithmetic and code pages                                            */
/* ------------------------------------------------------------------ */
/* round-half-away-from-zero of a*b/c; -1 on division by zero or overflow */
API(kernel32, INT, MulDiv, (INT a, INT b, INT c))
{
    if (!c)
        return -1;
    int64_t p = (int64_t)a * b;
    int neg = (p < 0) != (c < 0);
    uint64_t ap = p < 0 ? (uint64_t)-p : (uint64_t)p;
    uint64_t ac = c < 0 ? (uint64_t)-(int64_t)c : (uint64_t)c;
    uint64_t q = (ap + ac / 2) / ac;
    if (q > 0x7fffffffu)
        return -1;
    return neg ? -(INT)q : (INT)q;
}

/* The ANSI code page is UTF-8 (see PLAN-CORE §4 rule 5); CP_ACP, CP_OEMCP and CP_THREAD_ACP
 * name it. No other code page has a converter in Stage 1. */
static int cp_is_utf8(UINT cp) { return cp == 0 || cp == 1 || cp == 3 || cp == 65001; }

/* UTF-8 is not a double-byte code page, so no byte is a lead byte */
API(kernel32, BOOL, IsDBCSLeadByteEx, (UINT cp, BYTE ch))
{
    (void)cp;
    (void)ch;
    return FALSE;
}

API(kernel32, BOOL, IsDBCSLeadByte, (BYTE ch))
{
    return FALSE;
}

/* MB_ERR_INVALID_CHARS and MB_PRECOMPOSED are accepted and ignored: invalid UTF-8 bytes always
 * decode as Latin-1 (PLAN-CORE §4 rule 5). */
API(kernel32, INT, MultiByteToWideChar, (UINT cp, DWORD flags, const char *src, INT srclen, WCHAR *dst, INT dstlen))
{
    (void)flags;
    if (!cp_is_utf8(cp) || !src || !srclen || dstlen < 0) {
        aw_set_last_error(87);
        return 0;
    }
    if (srclen < 0)
        srclen = (INT)strlen(src) + 1; /* the terminator is converted and counted */
    INT need = aw_u2w_buf(src, srclen, NULL, 0);
    if (!dst || !dstlen)
        return need;
    if (need > dstlen) {
        aw_set_last_error(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    aw_u2w_buf(src, srclen, dst, dstlen);
    return need;
}

API(kernel32, INT, WideCharToMultiByte,
    (UINT cp, DWORD flags, const WCHAR *src, INT srclen, char *dst, INT dstlen, const char *defchar, BOOL *used))
{
    (void)flags;
    (void)defchar;
    if (used)
        *used = FALSE; /* UTF-8 encodes every UTF-16 unit, so the default character is never used */
    if (!cp_is_utf8(cp) || !src || !srclen || dstlen < 0) {
        aw_set_last_error(87);
        return 0;
    }
    if (srclen < 0)
        srclen = (INT)aw_wcslen(src) + 1;
    INT need = aw_w2u_buf(src, srclen, NULL, 0);
    if (!dst || !dstlen)
        return need;
    if (need > dstlen) {
        aw_set_last_error(122);
        return 0;
    }
    aw_w2u_buf(src, srclen, dst, dstlen);
    return need;
}

/* ------------------------------------------------------------------ */
/* exception filter storage (UnhandledExceptionFilter itself is package C) */
/* ------------------------------------------------------------------ */
API(kernel32, void *, SetUnhandledExceptionFilter, (void *filter))
{
    void *old = unhandled_filter;
    unhandled_filter = filter;
    return old;
}

/* ------------------------------------------------------------------ */
/* virtual memory: VirtualProtect / VirtualQuery                        */
/* The page table is read from /proc/self/maps, so answers reflect the  */
/* real mappings of the relinked image (MinGW pseudo-relocations call    */
/* VirtualProtect on their own sections and must see the change).       */
/* ------------------------------------------------------------------ */
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_EXECUTE_WRITECOPY 0x80u
#define MEM_COMMIT 0x1000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_IMAGE 0x1000000u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INVALID_ADDRESS 487u
#define PGSZ ((uintptr_t)sysconf(_SC_PAGESIZE))

/* x64 MEMORY_BASIC_INFORMATION */
typedef struct {
    void *BaseAddress, *AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MBI_X64;
_Static_assert(sizeof(MBI_X64) == 48, "MEMORY_BASIC_INFORMATION size");

struct mapent {
    uintptr_t start, end;
    int r, w, x;
    char *path; /* NULL for anonymous memory */
};
struct mapset {
    struct mapent *e;
    size_t n;
};

static void maps_load(struct mapset *m)
{
    size_t cap = 0;
    m->e = NULL;
    m->n = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        char perm[8] = {0}, path[4096] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %4095[^\n]", &s, &e, perm, path) < 3)
            continue;
        if (m->n == cap) {
            size_t nc = cap ? cap * 2 : 256;
            struct mapent *ne = realloc(m->e, nc * sizeof *ne);
            if (!ne)
                break;
            m->e = ne;
            cap = nc;
        }
        struct mapent *me = &m->e[m->n++];
        me->start = s;
        me->end = e;
        me->r = perm[0] == 'r';
        me->w = perm[1] == 'w';
        me->x = perm[2] == 'x';
        me->path = path[0] ? strdup(path) : NULL;
    }
    fclose(f);
}

static void maps_free(struct mapset *m)
{
    for (size_t i = 0; i < m->n; i++)
        free(m->e[i].path);
    free(m->e);
    m->e = NULL;
    m->n = 0;
}

static long maps_at(const struct mapset *m, uintptr_t a)
{
    for (size_t i = 0; i < m->n; i++)
        if (a >= m->e[i].start && a < m->e[i].end)
            return (long)i;
    return -1;
}

static int same_path(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return !strcmp(a, b);
}

static int same_prot(const struct mapent *a, const struct mapent *b)
{
    return a->r == b->r && a->w == b->w && a->x == b->x && same_path(a->path, b->path);
}

/* Windows protection constant of one mapping */
static DWORD win_prot_of(const struct mapent *e)
{
    if (e->x)
        return e->w ? PAGE_EXECUTE_READWRITE : e->r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (e->w)
        return PAGE_READWRITE;
    if (e->r)
        return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* mprotect flags of a Windows protection constant; -1 if invalid. PAGE_GUARD, PAGE_NOCACHE and
 * PAGE_WRITECOMBINE (0x100..0x400) are accepted and ignored. */
static int unix_prot_of(DWORD p)
{
    if (p & ~0x7ffu)
        return -1;
    switch (p & 0xffu) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return -1;
    }
}

/* File-backed mappings (the relinked executable and shared objects) are reported as images;
 * Stage 1 has no mapped-file views. Anonymous mappings are private. */
static DWORD mem_type_of(const char *path)
{
    return path && path[0] == '/' ? MEM_IMAGE : MEM_PRIVATE;
}

/* first index of the run of mappings that share entry i's protection and path */
static size_t prot_run_lo(const struct mapset *m, size_t i)
{
    size_t lo = i;
    while (lo > 0 && m->e[lo - 1].end == m->e[lo].start && same_prot(&m->e[lo - 1], &m->e[i]))
        lo--;
    return lo;
}

static size_t prot_run_hi(const struct mapset *m, size_t i)
{
    size_t hi = i;
    while (hi + 1 < m->n && m->e[hi].end == m->e[hi + 1].start && same_prot(&m->e[hi + 1], &m->e[i]))
        hi++;
    return hi;
}

/* first index of the allocation holding entry i: an image spans several protection runs of
 * the same file; anonymous memory is its own allocation */
static size_t alloc_lo(const struct mapset *m, size_t i)
{
    if (!m->e[i].path)
        return prot_run_lo(m, i);
    size_t lo = i;
    while (lo > 0 && m->e[lo - 1].end == m->e[lo].start && same_path(m->e[lo - 1].path, m->e[i].path))
        lo--;
    return lo;
}

API(kernel32, SIZE_T, VirtualQuery, (const void *addr, void *buf, SIZE_T len))
{
    if (!buf || len < sizeof(MBI_X64)) {
        aw_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    uintptr_t a = (uintptr_t)addr;
    uintptr_t pg = a & ~(PGSZ - 1);
    MBI_X64 mbi;
    memset(&mbi, 0, sizeof mbi);
    struct mapset m;
    maps_load(&m);
    long i = maps_at(&m, a);
    if (i < 0) {
        /* free region: from this page up to the next mapping above it */
        uintptr_t next = UINTPTR_MAX & ~(PGSZ - 1);
        for (size_t j = 0; j < m.n; j++)
            if (m.e[j].start > a && m.e[j].start < next)
                next = m.e[j].start;
        mbi.BaseAddress = (void *)pg;
        mbi.RegionSize = next - pg;
        mbi.State = MEM_FREE;
        mbi.Protect = PAGE_NOACCESS;
    } else {
        size_t lo = prot_run_lo(&m, (size_t)i), hi = prot_run_hi(&m, (size_t)i);
        size_t al = alloc_lo(&m, (size_t)i);
        mbi.BaseAddress = (void *)m.e[lo].start;
        mbi.AllocationBase = (void *)m.e[al].start;
        mbi.AllocationProtect = win_prot_of(&m.e[al]);
        mbi.RegionSize = m.e[hi].end - m.e[lo].start;
        mbi.State = MEM_COMMIT;
        mbi.Protect = win_prot_of(&m.e[i]);
        mbi.Type = mem_type_of(m.e[i].path);
    }
    maps_free(&m);
    memcpy(buf, &mbi, sizeof mbi);
    return sizeof mbi;
}

API(kernel32, BOOL, VirtualProtect, (void *addr, SIZE_T size, DWORD prot, DWORD *old))
{
    uintptr_t a = (uintptr_t)addr;
    int up = unix_prot_of(prot);
    if (!old || !size || up < 0 || a + size < a) {
        aw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uintptr_t pg = PGSZ;
    uintptr_t start = a & ~(pg - 1);
    uintptr_t end = (a + size + pg - 1) & ~(pg - 1);
    struct mapset m;
    maps_load(&m);
    long first = maps_at(&m, start);
    int ok = first >= 0;
    DWORD oldp = ok ? win_prot_of(&m.e[first]) : 0;
    for (uintptr_t cur = start; ok && cur < end;) {
        long j = maps_at(&m, cur);
        if (j < 0)
            ok = 0;
        else
            cur = m.e[j].end;
    }
    maps_free(&m);
    if (!ok) { /* the whole range must be mapped; nothing is changed otherwise */
        aw_set_last_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (mprotect((void *)start, end - start, up) != 0) {
        aw_set_last_error(winerr(errno));
        return FALSE;
    }
    *old = oldp;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* resources: aw_find_resource walks the image's .rsrc directory         */
/* ------------------------------------------------------------------ */
struct rname {
    int is_id;      /* MAKEINTRESOURCE value, or "#123" */
    uint32_t id;
    const WCHAR *s; /* otherwise a UTF-16 name, compared case-insensitively */
    size_t n;
};

static void rname_of(const void *p, struct rname *r)
{
    memset(r, 0, sizeof *r);
    if (IS_INTRESOURCE(p)) {
        r->is_id = 1;
        r->id = (uint32_t)(uintptr_t)p;
        return;
    }
    const WCHAR *w = p;
    size_t n = aw_wcslen(w);
    if (n > 1 && w[0] == '#') {
        uint32_t v = 0;
        size_t i = 1;
        for (; i < n && w[i] >= '0' && w[i] <= '9'; i++)
            v = v * 10 + (w[i] - '0');
        if (i == n) {
            r->is_id = 1;
            r->id = v;
            return;
        }
    }
    r->s = w;
    r->n = n;
}

static WCHAR fold_w(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }

/* the named directory entry at nameoff (high bit set) equals the wanted UTF-16 name */
static int res_name_is(const uint8_t *res, uint32_t nameoff, const struct rname *w)
{
    if (w->is_id)
        return 0;
    const uint8_t *p = res + (nameoff & 0x7fffffffu);
    uint16_t len;
    memcpy(&len, p, 2);
    if (len != w->n)
        return 0;
    for (size_t i = 0; i < len; i++) {
        WCHAR c;
        memcpy(&c, p + 2 + 2 * i, 2);
        if (fold_w(c) != fold_w(w->s[i]))
            return 0;
    }
    return 1;
}

/* looks w up in the resource directory at dir; on success *off is its OffsetToData */
static int res_find(const uint8_t *res, const uint8_t *lim, const uint8_t *dir, const struct rname *w, uint32_t *off)
{
    if (dir + 16 > lim)
        return 0;
    uint16_t named, ids;
    memcpy(&named, dir + 12, 2);
    memcpy(&ids, dir + 14, 2);
    const uint8_t *e = dir + 16;
    for (uint32_t i = 0; i < (uint32_t)named + ids; i++, e += 8) {
        if (e + 8 > lim)
            break;
        uint32_t name, o;
        memcpy(&name, e, 4);
        memcpy(&o, e + 4, 4);
        int hit = (name & 0x80000000u) ? res_name_is(res, name, w) : (w->is_id && name == w->id);
        if (hit) {
            *off = o;
            return 1;
        }
    }
    return 0;
}

void *aw_find_resource(void *module, const void *type, const void *name, uint32_t *size)
{
    if (module && module != (void *)__aw_image_base)
        return NULL; /* fake DLL modules carry no resources */
    uint8_t *base = (uint8_t *)__aw_image_base;
    uint32_t pe, ndirs, rva, rsz;
    memcpy(&pe, base + 0x3c, 4);
    uint8_t *opt = base + pe + 24;
    memcpy(&ndirs, opt + 108, 4);
    if (ndirs < 3)
        return NULL;
    memcpy(&rva, opt + 112 + 2 * 8, 4); /* data directory 2: resources */
    memcpy(&rsz, opt + 112 + 2 * 8 + 4, 4);
    if (!rva)
        return NULL;
    const uint8_t *res = base + rva, *lim = res + rsz;
    struct rname t, n;
    rname_of(type, &t);
    rname_of(name, &n);
    uint32_t off;
    if (!res_find(res, lim, res, &t, &off) || !(off & 0x80000000u))
        return NULL;
    const uint8_t *d = res + (off & 0x7fffffffu);
    if (!res_find(res, lim, d, &n, &off) || !(off & 0x80000000u))
        return NULL;
    d = res + (off & 0x7fffffffu);
    /* language: the first entry of the name directory */
    if (d + 16 + 8 > lim)
        return NULL;
    uint16_t named, ids;
    memcpy(&named, d + 12, 2);
    memcpy(&ids, d + 14, 2);
    if (!named && !ids)
        return NULL;
    uint32_t loff;
    memcpy(&loff, d + 16 + 4, 4);
    if (loff & 0x80000000u)
        return NULL; /* a directory where a data entry belongs */
    const uint8_t *de = res + loff;
    if (de + 16 > lim)
        return NULL;
    uint32_t drva, dsz;
    memcpy(&drva, de, 4);
    memcpy(&dsz, de + 4, 4);
    if (size)
        *size = dsz;
    return base + drva;
}
