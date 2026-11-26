/* msvcrt / ucrt C runtime (PLAN-CORE 3.2, package S2).
 *
 * FILE is the 48-byte msvcrt layout. The runtime keeps the glibc FILE* in _tmpfname and the fd
 * in _file. _cnt stays 0 so that the (inline) getc/putc paths always call _filbuf/_flsbuf.
 * Streams are binary (no CRLF). Names not in PLAN-CORE 3.2 but required by the default
 * mingw-w64 (UCRT) startup and stdio are exported too: __acrt_iob_func, __stdio_common_*,
 * _initialize_narrow_environment, _configure_narrow_argv, __p_* accessors, _crt_atexit,
 * _initterm_e, _set_new_mode, _configthreadlocale, _set_invalid_parameter_handler, setvbuf.
 * printf-family functions live in printf.c. */
#include "aw.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <immintrin.h>

extern void WINAPI __aw_kernel32_ExitProcess(UINT code);
extern LPSTR WINAPI __aw_kernel32_GetCommandLineA(void);
extern char *aw_format_tmp(const char *fmt, char *ap, int legacy_exp, char *scratch, size_t cap, size_t *out_len,
                           int *heap_used);

#define IOB (__aw_msvcrt__iob)
#define AW_IOFBF 0
#define AW_IOLBF 0x40
#define AW_IONBF 4
#define AW_IOREAD 0x1
#define AW_IOWRT 0x2
#define AW_IORW 0x80
#define EOF_CODE (-1)

typedef struct aw_file {
    char *_ptr;      /* 0  */
    int _cnt;        /* 8  */
    char *_base;     /* 16 */
    int _flag;       /* 24 */
    int _file;       /* 28: fd */
    int _charbuf;    /* 32 */
    int _bufsiz;     /* 36 */
    char *_tmpfname; /* 40: glibc FILE* (runtime side) */
} aw_file;
_Static_assert(sizeof(aw_file) == 48, "msvcrt FILE layout");
typedef aw_file aw_iob3[3];

typedef void(AWCALL *SIGPROC)(int);
#define AW_SIG_DFL ((SIGPROC)0)
#define AW_SIG_IGN ((SIGPROC)1)
#define AW_SIG_ERR ((SIGPROC)(intptr_t)-1)
#define AW_SIGABRT 22

/* struct tm as msvcrt declares it: the first nine glibc fields, no zone data */
struct win_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

/* struct lconv as msvcrt declares it, including the wide-string members */
struct win_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol;
    char *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space,
        p_sign_posn, n_sign_posn;
    WCHAR *_W_decimal_point, *_W_thousands_sep, *_W_int_curr_symbol, *_W_currency_symbol,
        *_W_mon_decimal_point, *_W_mon_thousands_sep, *_W_positive_sign, *_W_negative_sign;
};

/* ---- data exports ------------------------------------------------------- */

APIDATA(msvcrt, aw_iob3, _iob);
APIDATA(msvcrt, char *, _acmdln) = NULL;
APIDATA(msvcrt, char **, __initenv) = NULL;
APIDATA(msvcrt, int, _commode) = 0;
APIDATA(msvcrt, int, _fmode) = 0;

/* ---- internal state ----------------------------------------------------- */

static int inited;
static int aw_argc;
static char **aw_argv;
static int new_mode;
static int thread_locale;
static void *invalid_param_handler;
static void *usermatherr_fn;
static SIGPROC sig_tab[32];

static pthread_mutex_t atexit_lock = PTHREAD_MUTEX_INITIALIZER;
static VOIDPROC *atexit_fns;
static size_t atexit_n, atexit_cap;

static __thread struct win_tm win_tm_buf;

static WCHAR w_dot[2] = {'.', 0};
static WCHAR w_empty[1] = {0};
static struct win_lconv lc_c = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127,
    ._W_decimal_point = w_dot,
    ._W_thousands_sep = w_empty,
    ._W_int_curr_symbol = w_empty,
    ._W_currency_symbol = w_empty,
    ._W_mon_decimal_point = w_empty,
    ._W_mon_thousands_sep = w_empty,
    ._W_positive_sign = w_empty,
    ._W_negative_sign = w_empty,
};

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* MS command-line rules: 2n backslashes before a quote give n backslashes and toggle quoting;
 * 2n+1 give n backslashes and a literal quote; "" inside quotes is a literal quote. */
static char **parse_cmdline(const char *cl, int *out_argc)
{
    size_t len = strlen(cl);
    char **av = malloc((len / 2 + 2) * sizeof *av);
    int ac = 0;
    const char *p = cl;
    if (!av) {
        *out_argc = 0;
        return NULL;
    }
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *arg = malloc(len + 1);
        size_t n = 0;
        int inq = 0;
        if (!arg)
            break;
        for (;;) {
            if (*p == '\\') {
                size_t nb = 0;
                while (*p == '\\') {
                    nb++;
                    p++;
                }
                if (*p == '"') {
                    for (size_t i = 0; i < nb / 2; i++)
                        arg[n++] = '\\';
                    if (nb & 1)
                        arg[n++] = '"';
                    else
                        inq = !inq;
                    p++;
                } else {
                    for (size_t i = 0; i < nb; i++)
                        arg[n++] = '\\';
                }
                continue;
            }
            if (*p == '"') {
                if (inq && p[1] == '"') {
                    arg[n++] = '"';
                    p += 2;
                    continue;
                }
                inq = !inq;
                p++;
                continue;
            }
            if (!*p || (!inq && (*p == ' ' || *p == '\t')))
                break;
            arg[n++] = *p++;
        }
        arg[n] = 0;
        av[ac++] = arg;
    }
    av[ac] = NULL;
    *out_argc = ac;
    return av;
}

void aw_msvcrt_init(void)
{
    if (inited)
        return;
    inited = 1;

    IOB[0]._file = 0;
    IOB[0]._flag = AW_IOREAD;
    IOB[0]._tmpfname = (char *)stdin;
    IOB[1]._file = 1;
    IOB[1]._flag = AW_IOWRT;
    IOB[1]._tmpfname = (char *)stdout;
    IOB[2]._file = 2;
    IOB[2]._flag = AW_IOWRT;
    IOB[2]._tmpfname = (char *)stderr;
    for (int i = 0; i < 3; i++)
        IOB[i]._bufsiz = 4096;

    char *cl = __aw_kernel32_GetCommandLineA();
    __aw_msvcrt__acmdln = cl;
    aw_argv = parse_cmdline(cl ? cl : "", &aw_argc);
    __aw_msvcrt___initenv = environ;
}

/* Called by the runtime after the program's last stdio call and by ExitProcess paths. */
void aw_msvcrt_flush_all(void)
{
    fflush(NULL);
}

/* Hook for the SEH package: a Windows signal (SIGFPE, SIGILL, SIGSEGV, ...) raised by the
 * program. Returns 1 when the program handled it (a handler ran or the signal is ignored) and
 * 0 when the default action applies. A handler is reset to SIG_DFL before it runs (C semantics). */
int aw_msvcrt_fault_signal(int ws)
{
    if (ws <= 0 || ws >= 32)
        return 0;
    SIGPROC h = sig_tab[ws];
    if (h == AW_SIG_IGN)
        return 1;
    if (!h || h == AW_SIG_DFL)
        return 0;
    sig_tab[ws] = AW_SIG_DFL;
    h(ws);
    return 1;
}

/* ---- atexit / exit ------------------------------------------------------ */

static int atexit_add(VOIDPROC fn)
{
    int rc = 0;
    pthread_mutex_lock(&atexit_lock);
    if (atexit_n == atexit_cap) {
        size_t nc = atexit_cap ? atexit_cap * 2 : 32;
        VOIDPROC *na = realloc(atexit_fns, nc * sizeof *na);
        if (!na) {
            rc = -1;
            goto out;
        }
        atexit_fns = na;
        atexit_cap = nc;
    }
    atexit_fns[atexit_n++] = fn;
out:
    pthread_mutex_unlock(&atexit_lock);
    return rc;
}

/* Runs registered functions last-in first-out. Each one is removed before it runs. */
static void run_atexit(void)
{
    for (;;) {
        VOIDPROC fn = NULL;
        pthread_mutex_lock(&atexit_lock);
        if (atexit_n)
            fn = atexit_fns[--atexit_n];
        pthread_mutex_unlock(&atexit_lock);
        if (!fn)
            break;
        fn();
    }
}

API(msvcrt, int, atexit, (VOIDPROC fn))
{
    return fn ? atexit_add(fn) : -1;
}

API(msvcrt, int, _crt_atexit, (VOIDPROC fn))
{
    return fn ? atexit_add(fn) : -1;
}

API(msvcrt, void, exit, (int code))
{
    run_atexit();
    aw_msvcrt_flush_all();
    __aw_kernel32_ExitProcess((UINT)code);
}

API(msvcrt, void, _exit, (int code))
{
    __aw_kernel32_ExitProcess((UINT)code);
}

API(msvcrt, void, _Exit, (int code))
{
    __aw_kernel32_ExitProcess((UINT)code);
}

API(msvcrt, void, _cexit, (void))
{
    aw_msvcrt_flush_all();
}

API(msvcrt, void, _amsg_exit, (int rterrnum))
{
    fprintf(stderr, "\r\nruntime error R%d\r\n", rterrnum);
    aw_msvcrt_flush_all();
    _exit(255);
}

API(msvcrt, void, abort, (void))
{
    aw_msvcrt_fault_signal(AW_SIGABRT);
    _exit(3);
}

API(msvcrt, void, _initterm, (VOIDPROC *first, VOIDPROC *last))
{
    for (VOIDPROC *p = first; p && last && p < last; p++)
        if (*p)
            (*p)();
}

API(msvcrt, int, _initterm_e, (INTPROC *first, INTPROC *last))
{
    for (INTPROC *p = first; p && last && p < last; p++)
        if (*p) {
            int r = (*p)();
            if (r)
                return r;
        }
    return 0;
}

/* ---- signals ------------------------------------------------------------ */

static void host_tramp(int host)
{
    SIGPROC h = sig_tab[host];
    if (h && h != AW_SIG_IGN && h != AW_SIG_DFL)
        h(host);
}

/* Windows signal numbers: SIGINT 2, SIGILL 4, SIGFPE 8, SIGSEGV 11, SIGTERM 15, SIGBREAK 21,
 * SIGABRT 22. SIGINT and SIGTERM share their numbers with Linux and are forwarded to the program
 * handler. The fault signals belong to the SEH package (see aw_msvcrt_fault_signal). */
static int win_signal_ok(int sig)
{
    return sig == 2 || sig == 4 || sig == 8 || sig == 11 || sig == 15 || sig == 21 || sig == AW_SIGABRT;
}

API(msvcrt, SIGPROC, signal, (int sig, SIGPROC fn))
{
    if (!win_signal_ok(sig)) {
        errno = EINVAL;
        return AW_SIG_ERR;
    }
    SIGPROC old = sig_tab[sig];
    sig_tab[sig] = fn;
    if (sig == SIGINT || sig == SIGTERM) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        if (fn == AW_SIG_DFL)
            sa.sa_handler = SIG_DFL;
        else if (fn == AW_SIG_IGN)
            sa.sa_handler = SIG_IGN;
        else
            sa.sa_handler = host_tramp;
        sigaction(sig, &sa, NULL);
    }
    return old;
}

/* Default action of an unhandled Windows signal is termination with exit code 3. */
API(msvcrt, int, raise, (int sig))
{
    if (!win_signal_ok(sig)) {
        errno = EINVAL;
        return -1;
    }
    if (aw_msvcrt_fault_signal(sig))
        return 0;
    _exit(3);
}

/* ---- startup accessors -------------------------------------------------- */

API(msvcrt, int, __getmainargs, (int *pargc, char ***pargv, char ***penv, int expand, void *startinfo))
{
    (void)expand;
    (void)startinfo;
    aw_msvcrt_init();
    *pargc = aw_argc;
    *pargv = aw_argv;
    *penv = environ;
    return 0;
}

API(msvcrt, void, __set_app_type, (int type))
{
    (void)type;
}

API(msvcrt, void, _set_app_type, (int type))
{
    (void)type;
}

API(msvcrt, void, __setusermatherr, (void *fn))
{
    usermatherr_fn = fn;
}

API(msvcrt, int, _initialize_narrow_environment, (void))
{
    aw_msvcrt_init();
    return 0;
}

API(msvcrt, int, _configure_narrow_argv, (int mode))
{
    (void)mode;
    aw_msvcrt_init();
    return 0;
}

API(msvcrt, int *, __p___argc, (void))
{
    aw_msvcrt_init();
    return &aw_argc;
}

API(msvcrt, char ***, __p___argv, (void))
{
    aw_msvcrt_init();
    return &aw_argv;
}

API(msvcrt, char ***, __p__environ, (void))
{
    return &environ;
}

API(msvcrt, char **, __p__acmdln, (void))
{
    return &__aw_msvcrt__acmdln;
}

API(msvcrt, int *, __p__fmode, (void))
{
    return &__aw_msvcrt__fmode;
}

API(msvcrt, int *, __p__commode, (void))
{
    return &__aw_msvcrt__commode;
}

API(msvcrt, int, _set_new_mode, (int mode))
{
    int old = new_mode;
    new_mode = mode;
    return old;
}

API(msvcrt, int, _configthreadlocale, (int mode))
{
    int old = thread_locale;
    if (mode)
        thread_locale = mode;
    return old;
}

API(msvcrt, void *, _set_invalid_parameter_handler, (void *fn))
{
    void *old = invalid_param_handler;
    invalid_param_handler = fn;
    return old;
}

API(msvcrt, int *, _errno, (void))
{
    return &errno;
}

/* ---- stdio -------------------------------------------------------------- */

API(msvcrt, aw_file *, __iob_func, (void))
{
    return IOB;
}

API(msvcrt, aw_file *, __acrt_iob_func, (unsigned idx))
{
    return idx < 3 ? &IOB[idx] : NULL;
}

static FILE *gf(aw_file *f)
{
    return f ? (FILE *)f->_tmpfname : NULL;
}

static int file_fmt(aw_file *f, const char *fmt, char *ap, int legacy_exp)
{
    FILE *g = gf(f);
    size_t n;
    int heap;
    if (!g)
        return -1;
    char scratch[256];
    char *s = aw_format_tmp(fmt, ap, legacy_exp, scratch, sizeof scratch, &n, &heap);
    if (!s)
        return -1;
    int r = -1;
    if (fwrite(s, 1, n, g) == n)
        r = (int)n;
    if (heap)
        free(s);
    return r;
}

API(msvcrt, int, printf, (const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = file_fmt(IOB + 1, fmt, (char *)ap, 1);
    __builtin_ms_va_end(ap);
    return r;
}

API(msvcrt, int, vprintf, (const char *fmt, char *ap))
{
    return file_fmt(IOB + 1, fmt, ap, 1);
}

API(msvcrt, int, fprintf, (aw_file *f, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = file_fmt(f, fmt, (char *)ap, 1);
    __builtin_ms_va_end(ap);
    return r;
}

API(msvcrt, int, vfprintf, (aw_file *f, const char *fmt, char *ap))
{
    return file_fmt(f, fmt, ap, 1);
}

/* ucrt: options bit 0x18 selects msvcrt-compatible three-digit exponents */
API(msvcrt, int, __stdio_common_vfprintf, (uint64_t options, aw_file *f, const char *fmt, void *locale, char *ap))
{
    (void)locale;
    return file_fmt(f, fmt, ap, (options & 0x18) != 0);
}

API(msvcrt, int, fputs, (const char *s, aw_file *f))
{
    FILE *g = gf(f);
    return g ? fputs(s, g) : EOF_CODE;
}

API(msvcrt, int, fputc, (int c, aw_file *f))
{
    FILE *g = gf(f);
    return g ? fputc(c, g) : EOF_CODE;
}

API(msvcrt, int, putchar, (int c))
{
    return putchar(c);
}

API(msvcrt, int, puts, (const char *s))
{
    return puts(s);
}

API(msvcrt, size_t, fwrite, (const void *p, size_t sz, size_t n, aw_file *f))
{
    FILE *g = gf(f);
    return g ? fwrite(p, sz, n, g) : 0;
}

API(msvcrt, size_t, fread, (void *p, size_t sz, size_t n, aw_file *f))
{
    FILE *g = gf(f);
    return g ? fread(p, sz, n, g) : 0;
}

API(msvcrt, int, fflush, (aw_file *f))
{
    if (!f)
        return fflush(NULL);
    FILE *g = gf(f);
    return g ? fflush(g) : EOF_CODE;
}

API(msvcrt, int, setvbuf, (aw_file *f, char *buf, int mode, size_t size))
{
    FILE *g = gf(f);
    if (!g)
        return -1;
    int m = mode == AW_IOLBF ? _IOLBF : mode == AW_IONBF ? _IONBF : _IOFBF;
    return setvbuf(g, buf, m, size);
}

/* inline getc/putc in mingw headers call these when _cnt is exhausted (always, here) */
API(msvcrt, int, _filbuf, (aw_file *f))
{
    FILE *g = gf(f);
    f->_cnt = 0;
    return g ? fgetc(g) : EOF_CODE;
}

API(msvcrt, int, _flsbuf, (int c, aw_file *f))
{
    FILE *g = gf(f);
    f->_cnt = 0;
    return g ? fputc(c, g) : EOF_CODE;
}

API(msvcrt, aw_file *, fopen, (const char *path, const char *mode))
{
    if (!path || !mode)
        return NULL;
    char base = mode[0];
    if (base != 'r' && base != 'w' && base != 'a') {
        errno = EINVAL;
        return NULL;
    }
    int plus = strchr(mode, '+') != NULL;
    char gmode[3] = {base, plus ? '+' : 0, 0};
    char *up = aw_unix_path(path);
    if (!up)
        return NULL;
    FILE *g = fopen(up, gmode);
    TRACE("fopen \"%s\" mode=%s -> %s", up, mode, g ? "ok" : strerror(errno));
    free(up);
    if (!g)
        return NULL;
    aw_file *f = calloc(1, sizeof *f);
    if (!f) {
        fclose(g);
        return NULL;
    }
    f->_file = fileno(g);
    f->_flag = plus ? AW_IORW : (base == 'r' ? AW_IOREAD : AW_IOWRT);
    f->_bufsiz = 4096;
    f->_tmpfname = (char *)g;
    return f;
}

API(msvcrt, int, fclose, (aw_file *f))
{
    FILE *g = gf(f);
    if (!g)
        return EOF_CODE;
    int r = fclose(g);
    if (f >= IOB && f < IOB + 3) {
        f->_tmpfname = NULL;
        f->_flag = 0;
    } else {
        free(f);
    }
    return r;
}

/* ---- locale ------------------------------------------------------------- */

API(msvcrt, struct win_lconv *, localeconv, (void))
{
    return &lc_c;
}

API(msvcrt, unsigned int, ___lc_codepage_func, (void))
{
    return 65001; /* ANSI code page is UTF-8 */
}

API(msvcrt, int, ___mb_cur_max_func, (void))
{
    return 1;
}

API(msvcrt, int, _ismbblead, (unsigned int c))
{
    (void)c;
    return 0;
}

/* ---- time --------------------------------------------------------------- */

API(msvcrt, int64_t, _time64, (int64_t *t))
{
    time_t now = time(NULL);
    if (t)
        *t = (int64_t)now;
    return (int64_t)now;
}

API(msvcrt, struct win_tm *, _localtime64, (const int64_t *t))
{
    if (!t || *t < 0 || *t > 32535215999LL)
        return NULL;
    time_t tt = (time_t)*t;
    struct tm g;
    if (!localtime_r(&tt, &g))
        return NULL;
    win_tm_buf.tm_sec = g.tm_sec;
    win_tm_buf.tm_min = g.tm_min;
    win_tm_buf.tm_hour = g.tm_hour;
    win_tm_buf.tm_mday = g.tm_mday;
    win_tm_buf.tm_mon = g.tm_mon;
    win_tm_buf.tm_year = g.tm_year;
    win_tm_buf.tm_wday = g.tm_wday;
    win_tm_buf.tm_yday = g.tm_yday;
    win_tm_buf.tm_isdst = g.tm_isdst;
    return &win_tm_buf;
}

API(msvcrt, size_t, strftime, (char *s, size_t max, const char *fmt, const struct win_tm *w))
{
    struct tm g;
    memset(&g, 0, sizeof g);
    g.tm_sec = w->tm_sec;
    g.tm_min = w->tm_min;
    g.tm_hour = w->tm_hour;
    g.tm_mday = w->tm_mday;
    g.tm_mon = w->tm_mon;
    g.tm_year = w->tm_year;
    g.tm_wday = w->tm_wday;
    g.tm_yday = w->tm_yday;
    g.tm_isdst = w->tm_isdst;
    /* zone offset and name for %z %Z come from the local zone for these fields */
    struct tm norm = g;
    g.tm_zone = "";
    if (mktime(&norm) != (time_t)-1) {
        g.tm_gmtoff = norm.tm_gmtoff;
        g.tm_zone = norm.tm_zone;
    }
    return strftime(s, max, fmt, &g);
}

/* ---- heap --------------------------------------------------------------- */

API(msvcrt, void *, malloc, (size_t n))
{
    return malloc(n);
}

API(msvcrt, void *, calloc, (size_t n, size_t m))
{
    return calloc(n, m);
}

API(msvcrt, void *, realloc, (void *p, size_t n))
{
    return realloc(p, n);
}

API(msvcrt, void, free, (void *p))
{
    free(p);
}

/* ---- memory and strings ------------------------------------------------- */

/* Glibc's generic memcpy variant is slow for mid-size copies (roughly 0.5-2 MiB here): its
 * ifunc variant is tuned for other sizes. A short AVX2 loop is up to twice as fast in that
 * range; outside it glibc wins, so only take over there. */
#define MEMCPY_AVX2_MIN (512u << 10)
#define MEMCPY_AVX2_MAX (2u << 20)

__attribute__((target("avx2"))) static void *memcpy_avx2(void *d, const void *s, size_t n)
{
    char *dd = d;
    const char *ss = s;
    size_t head = (32 - ((uintptr_t)dd & 31)) & 31;
    if (head > n)
        head = n;
    for (size_t i = 0; i < head; i++)
        dd[i] = ss[i];
    size_t i = head;
    for (; i + 32 <= n; i += 32)
        _mm256_store_si256((__m256i *)(dd + i), _mm256_loadu_si256((const __m256i *)(ss + i)));
    for (; i < n; i++)
        dd[i] = ss[i];
    return d;
}

static int have_avx2(void)
{
    static int cached = -1;
    if (cached < 0) {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    return cached;
}

API(msvcrt, void *, memcpy, (void *d, const void *s, size_t n))
{
    if (n >= MEMCPY_AVX2_MIN && n <= MEMCPY_AVX2_MAX && have_avx2())
        return memcpy_avx2(d, s, n);
    return memcpy(d, s, n);
}

API(msvcrt, void *, memmove, (void *d, const void *s, size_t n))
{
    return memmove(d, s, n);
}

API(msvcrt, void *, memset, (void *d, int c, size_t n))
{
    return memset(d, c, n);
}

API(msvcrt, int, memcmp, (const void *a, const void *b, size_t n))
{
    return memcmp(a, b, n);
}

API(msvcrt, size_t, strlen, (const char *s))
{
    return strlen(s);
}

API(msvcrt, size_t, wcslen, (const WCHAR *s))
{
    return aw_wcslen(s);
}

API(msvcrt, char *, strcpy, (char *d, const char *s))
{
    return strcpy(d, s);
}

API(msvcrt, char *, strncpy, (char *d, const char *s, size_t n))
{
    return strncpy(d, s, n);
}

API(msvcrt, char *, strcat, (char *d, const char *s))
{
    return strcat(d, s);
}

API(msvcrt, int, strcmp, (const char *a, const char *b))
{
    return strcmp(a, b);
}

API(msvcrt, int, strncmp, (const char *a, const char *b, size_t n))
{
    return strncmp(a, b, n);
}

API(msvcrt, char *, strstr, (const char *h, const char *n))
{
    return (char *)strstr(h, n);
}

API(msvcrt, char *, strchr, (const char *s, int c))
{
    return (char *)strchr(s, c);
}

API(msvcrt, char *, strrchr, (const char *s, int c))
{
    return (char *)strrchr(s, c);
}

API(msvcrt, char *, strerror, (int err))
{
    return strerror(err);
}

API(msvcrt, char *, _strlwr, (char *s))
{
    for (char *p = s; *p; p++)
        *p = (char)ascii_lower((unsigned char)*p);
    return s;
}

API(msvcrt, int, _stricmp, (const char *a, const char *b))
{
    for (;; a++, b++) {
        int x = ascii_lower((unsigned char)*a), y = ascii_lower((unsigned char)*b);
        if (x != y)
            return x - y;
        if (!x)
            return 0;
    }
}

API(msvcrt, int, toupper, (int c))
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

API(msvcrt, int, tolower, (int c))
{
    return ascii_lower(c);
}

/* Windows env names are case-insensitive; values are returned in place (no copy). */
API(msvcrt, char *, getenv, (const char *name))
{
    if (!name || !*name || strchr(name, '='))
        return NULL;
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++) {
        size_t i = 0;
        while (i < n && (*e)[i] && ascii_lower((unsigned char)(*e)[i]) == ascii_lower((unsigned char)name[i]))
            i++;
        if (i == n && (*e)[n] == '=')
            return *e + n + 1;
    }
    return NULL;
}
