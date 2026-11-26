/* printf engine with Windows CRT semantics (PLAN-CORE 3.2, package S2).
 * Arguments come from a Win64 argument-slot pointer: every variadic argument occupies one 8-byte
 * slot (integers in the low bytes, doubles as 8-byte IEEE values). A va_list received from the
 * program is such a pointer. The engine renders into a heap buffer; the callers copy or write it.
 *
 * Conversions: d i u o x X c C s S Z p n e E f F g G a A %; flags - + space # 0; width and
 * precision (also *); length h hh l ll L I I32 I64 z t j w. Narrow output is UTF-8, so %S and
 * %ls (UTF-16 strings) are converted. Floats use glibc snprintf per conversion; inf/nan use the
 * legacy selects msvcrt behaviour (three-digit exponent e+000, 1.#INF00 / -1.#IND00 spelling);
 * UCRT output (the default) uses two-digit exponents and inf / -nan(ind). */
#include "aw.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>

/* ---- output buffer ------------------------------------------------------ */

typedef struct {
    char *p;      /* current buffer: the caller's scratch area or the heap block */
    size_t n, cap;
    char *heap;   /* set once the output outgrows the scratch area */
    int oom;
} sbuf;

static void sb_init(sbuf *b, char *scratch, size_t cap)
{
    b->p = scratch;
    b->n = 0;
    b->cap = scratch ? cap : 0;
    b->heap = NULL;
    b->oom = 0;
}

static void sb_reserve(sbuf *b, size_t add)
{
    if (b->oom)
        return;
    size_t need = b->n + add + 1; /* keep room for a terminating nul */
    if (need <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need)
        nc *= 2;
    char *np;
    if (b->heap) {
        np = realloc(b->heap, nc);
    } else {
        np = malloc(nc);
        if (np && b->p)
            memcpy(np, b->p, b->n);
    }
    if (!np) {
        b->oom = 1;
        return;
    }
    b->heap = b->p = np;
    b->cap = nc;
}

static void sb_put(sbuf *b, const char *s, size_t len)
{
    if (!len)
        return;
    sb_reserve(b, len);
    if (b->oom)
        return;
    memcpy(b->p + b->n, s, len);
    b->n += len;
}

static void sb_fill(sbuf *b, char c, size_t cnt)
{
    if (!cnt)
        return;
    sb_reserve(b, cnt);
    if (b->oom)
        return;
    memset(b->p + b->n, c, cnt);
    b->n += cnt;
}

/* ---- argument slots ------------------------------------------------------ */

static uint64_t slot_u64(char **ap)
{
    uint64_t v;
    memcpy(&v, *ap, 8);
    *ap += 8;
    return v;
}

static int32_t slot_i32(char **ap)
{
    int32_t v;
    memcpy(&v, *ap, 4); /* low half of the slot; the upper bytes are not defined */
    *ap += 8;
    return v;
}

static double slot_f64(char **ap)
{
    double d;
    memcpy(&d, *ap, 8);
    *ap += 8;
    return d;
}

/* ---- conversion state ---------------------------------------------------- */

struct spec {
    int left, plus, space, alt, zero;
    int width;
    int prec; /* -1 when absent */
    int bits; /* 8, 16, 32 or 64: width of the integer argument */
    int wide; /* 'l' prefix: %lc %ls */
    int narrow; /* 'h' prefix: %hs */
};

/* Pads one field. zero_ok says whether the 0 flag applies to this conversion. */
static void emit(sbuf *b, const struct spec *sp, const char *pre, size_t plen, const char *body, size_t blen,
                 int zero_ok)
{
    size_t tot = plen + blen;
    size_t pad = (size_t)sp->width > tot ? (size_t)sp->width - tot : 0;
    if (sp->left) {
        sb_put(b, pre, plen);
        sb_put(b, body, blen);
        sb_fill(b, ' ', pad);
    } else if (zero_ok && sp->zero) {
        sb_put(b, pre, plen);
        sb_fill(b, '0', pad);
        sb_put(b, body, blen);
    } else {
        sb_fill(b, ' ', pad);
        sb_put(b, pre, plen);
        sb_put(b, body, blen);
    }
}

static void fmt_int(sbuf *b, const struct spec *sp, uint64_t raw, int is_signed, int conv)
{
    char tmp[80];
    size_t i = sizeof tmp;
    uint64_t mag, orig;
    int neg = 0;

    if (is_signed) {
        int64_t v;
        switch (sp->bits) {
        case 8: v = (int8_t)raw; break;
        case 16: v = (int16_t)raw; break;
        case 32: v = (int32_t)raw; break;
        default: v = (int64_t)raw; break;
        }
        neg = v < 0;
        mag = neg ? 0 - (uint64_t)v : (uint64_t)v;
    } else {
        switch (sp->bits) {
        case 8: mag = (uint8_t)raw; break;
        case 16: mag = (uint16_t)raw; break;
        case 32: mag = (uint32_t)raw; break;
        default: mag = raw; break;
        }
    }
    orig = mag;

    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char *dig = (conv == 'X' || conv == 'p') ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[--i] = dig[mag % base];
        mag /= base;
    } while (mag);
    size_t nd = sizeof tmp - i;

    size_t zeros = 0;
    if (sp->prec >= 0) {
        if (sp->prec == 0 && orig == 0)
            nd = 0;
        else if ((size_t)sp->prec > nd)
            zeros = (size_t)sp->prec - nd;
    }
    if (sp->alt && conv == 'o' && zeros == 0 && (nd == 0 || tmp[i] != '0'))
        zeros = 1;

    char pre[4];
    size_t plen = 0;
    if (is_signed) {
        if (neg)
            pre[plen++] = '-';
        else if (sp->plus)
            pre[plen++] = '+';
        else if (sp->space)
            pre[plen++] = ' ';
    }
    if (sp->alt && orig != 0 && (conv == 'x' || conv == 'X')) {
        pre[plen++] = '0';
        pre[plen++] = conv;
    }

    size_t total = plen + zeros + nd;
    size_t pad = (size_t)sp->width > total ? (size_t)sp->width - total : 0;
    int zero_pad = sp->zero && !sp->left && sp->prec < 0;
    if (!sp->left && !zero_pad)
        sb_fill(b, ' ', pad);
    sb_put(b, pre, plen);
    if (!sp->left && zero_pad)
        sb_fill(b, '0', pad);
    sb_fill(b, '0', zeros);
    sb_put(b, tmp + i, nd);
    if (sp->left)
        sb_fill(b, ' ', pad);
}

static const uint64_t pow10_u64[20] = {1ull,
                                       10ull,
                                       100ull,
                                       1000ull,
                                       10000ull,
                                       100000ull,
                                       1000000ull,
                                       10000000ull,
                                       100000000ull,
                                       1000000000ull,
                                       10000000000ull,
                                       100000000000ull,
                                       1000000000000ull,
                                       10000000000000ull,
                                       100000000000000ull,
                                       1000000000000000ull,
                                       10000000000000000ull,
                                       100000000000000000ull,
                                       1000000000000000000ull,
                                       10000000000000000000ull};

/* Fast path for %f: exact decimal expansion of the binary value using integer arithmetic.
 * Handles precisions 0..9 when the rounded scaled value fits in 64 bits; other cases go to
 * the glibc fallback. Produces the sign followed by the digits, like snprintf does. */
static int fmt_float_fixed(char *body, size_t cap, const struct spec *sp, double v)
{
    int prec = sp->prec < 0 ? 6 : sp->prec;
    if (prec > 9)
        return -1;

    uint64_t bits;
    memcpy(&bits, &v, 8);
    int neg = (int)(bits >> 63);
    int be = (int)((bits >> 52) & 0x7ff);
    uint64_t mant = bits & ((1ull << 52) - 1);
    int e;
    if (!be) {
        e = -1074;
    } else {
        mant |= 1ull << 52;
        e = be - 1023 - 52;
    }

    uint64_t q;
    if (!mant) {
        q = 0;
    } else if (e >= 0) {
        if (e > 20 || mant > (UINT64_MAX >> e))
            return -1;
        uint64_t iv = mant << e;
        if (iv > UINT64_MAX / pow10_u64[prec])
            return -1;
        q = iv * pow10_u64[prec];
    } else {
        int k = -e;
        __uint128_t num = (__uint128_t)mant * pow10_u64[prec];
        if (k >= 128) {
            q = 0; /* value is far below half of the last digit */
        } else {
            if (k < 64 && (num >> k) > UINT64_MAX)
                return -1;
            q = (uint64_t)(num >> k);
            __uint128_t rem = num & (((__uint128_t)1 << k) - 1);
            __uint128_t half = (__uint128_t)1 << (k - 1);
            if (rem > half || (rem == half && (q & 1))) {
                if (q == UINT64_MAX)
                    return -1;
                q++;
            }
        }
    }

    char tmp[48];
    char *end = tmp + sizeof tmp, *w = end;
    uint64_t divisor = pow10_u64[prec];
    uint64_t ip = q / divisor, fp = q % divisor;
    for (int i = 0; i < prec; i++) {
        *--w = (char)('0' + fp % 10);
        fp /= 10;
    }
    if (prec > 0 || sp->alt)
        *--w = '.';
    do {
        *--w = (char)('0' + ip % 10);
        ip /= 10;
    } while (ip);
    if (neg)
        *--w = '-';
    else if (sp->plus)
        *--w = '+';
    else if (sp->space)
        *--w = ' ';
    size_t n = (size_t)(end - w);
    if (n > cap)
        return -1;
    memcpy(body, w, n);
    return (int)n;
}

static void fmt_float(sbuf *b, const struct spec *sp, double v, int conv, int legacy)
{
    if (isnan(v) || isinf(v)) {
        const char *t;
        char pre[2] = {0, 0};
        int upper = conv == 'E' || conv == 'F' || conv == 'G' || conv == 'A';
        if (legacy) {
            if (isnan(v))
                t = signbit(v) ? "-1.#IND00" : "1.#QNAN0";
            else
                t = signbit(v) ? "-1.#INF00" : "1.#INF00";
        } else if (isnan(v)) {
            t = signbit(v) ? "-nan(ind)" : "nan";
        } else {
            t = signbit(v) ? "-inf" : "inf";
        }
        if (!isnan(v) && !signbit(v) && sp->plus)
            pre[0] = '+';
        char spelled[16];
        size_t tl = strlen(t);
        memcpy(spelled, t, tl + 1);
        if (upper && !legacy)
            for (size_t i = 0; i < tl; i++)
                spelled[i] = (char)toupper((unsigned char)spelled[i]);
        emit(b, sp, pre, pre[0] ? 1 : 0, spelled, tl, 0);
        return;
    }

    if (conv == 'f' || conv == 'F') {
        char fixed[48];
        int n = fmt_float_fixed(fixed, sizeof fixed, sp, v);
        if (n >= 0) {
            size_t plen = (fixed[0] == '-' || fixed[0] == '+' || fixed[0] == ' ') ? 1 : 0;
            emit(b, sp, fixed, plen, fixed + plen, (size_t)n - plen, 1);
            return;
        }
    }

    char f[32];
    size_t k = 0;
    f[k++] = '%';
    if (sp->plus)
        f[k++] = '+';
    if (sp->space)
        f[k++] = ' ';
    if (sp->alt)
        f[k++] = '#';
    if (sp->prec >= 0) {
        char d[12];
        size_t nd = 0;
        unsigned p = (unsigned)sp->prec;
        do {
            d[nd++] = (char)('0' + p % 10);
            p /= 10;
        } while (p);
        f[k++] = '.';
        while (nd)
            f[k++] = d[--nd];
    }
    f[k++] = (char)conv;
    f[k] = 0;

    char stack[512];
    int need = snprintf(stack, sizeof stack, f, v);
    if (need < 0) {
        b->oom = 1;
        return;
    }
    /* the legacy exponent fixup below inserts one more digit */
    int fixup = legacy && (conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G');
    char *body = stack;
    if ((size_t)need + (fixup ? 2 : 1) > sizeof stack) {
        body = malloc((size_t)need + 2);
        if (!body) {
            b->oom = 1;
            return;
        }
        snprintf(body, (size_t)need + 1, f, v);
    }

    if (fixup) {
        char *e = strpbrk(body, "eE");
        if (e && (e[1] == '+' || e[1] == '-') && isdigit((unsigned char)e[2]) && isdigit((unsigned char)e[3]) &&
            !isdigit((unsigned char)e[4])) {
            memmove(e + 3, e + 2, strlen(e + 2) + 1);
            e[2] = '0';
        }
    }

    size_t plen = (body[0] == '-' || body[0] == '+' || body[0] == ' ') ? 1 : 0;
    emit(b, sp, body, plen, body + plen, strlen(body + plen), 1);
    if (body != stack)
        free(body);
}

static void fmt_narrow(sbuf *b, const struct spec *sp, const char *s)
{
    if (!s)
        s = "(null)";
    size_t n = sp->prec >= 0 ? strnlen(s, (size_t)sp->prec) : strlen(s);
    emit(b, sp, "", 0, s, n, 0);
}

static void fmt_wide(sbuf *b, const struct spec *sp, const WCHAR *w)
{
    if (!w) {
        fmt_narrow(b, sp, NULL);
        return;
    }
    size_t wl = aw_wcslen(w);
    if (sp->prec >= 0 && (size_t)sp->prec < wl)
        wl = (size_t)sp->prec;
    char *u = aw_w2u(w, (int)wl);
    if (!u) {
        b->oom = 1;
        return;
    }
    emit(b, sp, "", 0, u, strlen(u), 0);
    free(u);
}

static void fmt_wchar(sbuf *b, const struct spec *sp, WCHAR wc)
{
    char u[8];
    int n = aw_w2u_buf(&wc, 1, u, (int)sizeof u);
    if (n < 0 || n > (int)sizeof u)
        n = 0;
    emit(b, sp, "", 0, u, (size_t)n, 0);
}

/* ---- the engine ---------------------------------------------------------- */

/* Renders fmt with the arguments at *ap into b. Returns b->p nul-terminated (length in
 * *out_len), or NULL with *out_len == (size_t)-1 on failure. Any heap block allocated
 * while formatting is kept and must be freed by the caller (b->heap). */
static char *format_engine(const char *f, char *ap, int legacy, sbuf *b, size_t *out_len)
{
    if (!f) {
        *out_len = (size_t)-1;
        return NULL;
    }

    while (*f && !b->oom) {
        if (*f != '%') {
            const char *s = f;
            while (*f && *f != '%')
                f++;
            sb_put(b, s, (size_t)(f - s));
            continue;
        }
        const char *start = f++;
        if (*f == '%') {
            sb_put(b, "%", 1);
            f++;
            continue;
        }

        struct spec sp;
        memset(&sp, 0, sizeof sp);
        sp.prec = -1;
        sp.bits = 32;

        for (;; f++) {
            if (*f == '-')
                sp.left = 1;
            else if (*f == '+')
                sp.plus = 1;
            else if (*f == ' ')
                sp.space = 1;
            else if (*f == '#')
                sp.alt = 1;
            else if (*f == '0')
                sp.zero = 1;
            else
                break;
        }
        if (*f == '*') {
            f++;
            int w = slot_i32(&ap);
            if (w < 0) {
                sp.left = 1;
                w = -w;
            }
            sp.width = w;
        } else {
            while (*f >= '0' && *f <= '9') {
                if (sp.width < 100000000)
                    sp.width = sp.width * 10 + (*f - '0');
                f++;
            }
        }
        if (*f == '.') {
            f++;
            if (*f == '*') {
                f++;
                int p = slot_i32(&ap);
                sp.prec = p < 0 ? -1 : p;
            } else {
                sp.prec = 0;
                while (*f >= '0' && *f <= '9') {
                    if (sp.prec < 100000000)
                        sp.prec = sp.prec * 10 + (*f - '0');
                    f++;
                }
            }
        }

        for (;;) {
            if (f[0] == 'h' && f[1] == 'h') {
                sp.bits = 8;
                f += 2;
            } else if (f[0] == 'h') {
                sp.bits = 16;
                sp.narrow = 1;
                f++;
            } else if (f[0] == 'l' && f[1] == 'l') {
                sp.bits = 64;
                f += 2;
            } else if (f[0] == 'l') {
                sp.bits = 32;
                sp.wide = 1;
                f++;
            } else if (f[0] == 'L') {
                sp.bits = 64;
                f++;
            } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
                sp.bits = 64;
                f += 3;
            } else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') {
                sp.bits = 32;
                f += 3;
            } else if (f[0] == 'I') {
                sp.bits = 64;
                f++;
            } else if (f[0] == 'z' || f[0] == 't' || f[0] == 'j') {
                sp.bits = 64;
                f++;
            } else if (f[0] == 'w') {
                f++; /* accepted, no effect */
            } else {
                break;
            }
        }

        char conv = *f;
        if (!conv)
            break;
        f++;
        switch (conv) {
        case 'd':
        case 'i':
            fmt_int(b, &sp, slot_u64(&ap), 1, conv);
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            fmt_int(b, &sp, slot_u64(&ap), 0, conv);
            break;
        case 'p':
            sp.prec = 16;
            sp.zero = 0;
            fmt_int(b, &sp, slot_u64(&ap), 0, 'p');
            break;
        case 'c':
            if (sp.wide) {
                WCHAR wc = (WCHAR)slot_u64(&ap);
                fmt_wchar(b, &sp, wc);
            } else {
                char c = (char)slot_u64(&ap);
                emit(b, &sp, "", 0, &c, 1, 0);
            }
            break;
        case 'C':
            fmt_wchar(b, &sp, (WCHAR)slot_u64(&ap));
            break;
        case 's':
            if (sp.wide && !sp.narrow)
                fmt_wide(b, &sp, (const WCHAR *)(uintptr_t)slot_u64(&ap));
            else
                fmt_narrow(b, &sp, (const char *)(uintptr_t)slot_u64(&ap));
            break;
        case 'S':
            fmt_wide(b, &sp, (const WCHAR *)(uintptr_t)slot_u64(&ap));
            break;
        case 'Z':
            fmt_narrow(b, &sp, (const char *)(uintptr_t)slot_u64(&ap));
            break;
        case 'n': {
            void *p = (void *)(uintptr_t)slot_u64(&ap);
            if (p) {
                if (sp.bits == 64) {
                    int64_t v = (int64_t)b->n;
                    memcpy(p, &v, 8);
                } else {
                    int32_t v = (int32_t)b->n;
                    memcpy(p, &v, 4);
                }
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            fmt_float(b, &sp, slot_f64(&ap), conv, legacy);
            break;
        default:
            /* unknown conversion: copied through unchanged */
            sb_put(b, start, (size_t)(f - start));
            break;
        }
    }

    if (b->oom) {
        free(b->heap);
        b->heap = b->p = NULL;
        *out_len = (size_t)-1;
        return NULL;
    }
    sb_reserve(b, 0); /* terminating nul */
    if (b->oom) {
        free(b->heap);
        b->heap = b->p = NULL;
        *out_len = (size_t)-1;
        return NULL;
    }
    if (!b->p) {
        b->p = b->heap = malloc(1);
        if (!b->p) {
            *out_len = (size_t)-1;
            return NULL;
        }
    }
    b->p[b->n] = 0;
    *out_len = b->n;
    return b->p;
}

/* Formats using scratch when the result fits there, otherwise a heap block. *heap_used says
 * whether the returned pointer must be freed. */
char *aw_format_tmp(const char *f, char *ap, int legacy, char *scratch, size_t cap, size_t *out_len,
                    int *heap_used)
{
    sbuf b;
    sb_init(&b, scratch, cap);
    char *r = format_engine(f, ap, legacy, &b, out_len);
    *heap_used = b.heap != NULL;
    return r;
}

/* Formats into a heap buffer that the caller frees. */
char *aw_format_alloc(const char *f, char *ap, int legacy, size_t *out_len)
{
    int heap;
    return aw_format_tmp(f, ap, legacy, NULL, 0, out_len, &heap);
}

/* Copies a formatted result into a caller buffer of cap bytes (nul-terminated when cap > 0).
 * C99 mode returns the full length; legacy mode returns -1 when the result did not fit. */
static int store_bounded(char *buf, size_t cap, const char *s, size_t n, int c99)
{
    int fits = n < cap; /* n characters plus the nul */
    if (cap) {
        if (fits) {
            memcpy(buf, s, n);
            buf[n] = 0;
        } else if (c99) {
            memcpy(buf, s, cap - 1);
            buf[cap - 1] = 0;
        } else {
            memcpy(buf, s, cap); /* legacy: filled, not terminated */
        }
    }
    if (!fits && !c99)
        return -1;
    return (int)n;
}

/* ---- exports ------------------------------------------------------------ */

/* Formats into a caller buffer of cap bytes; large results spill to the heap. */
static int format_bounded(char *buf, size_t cap, const char *fmt, char *ap, int legacy, int c99)
{
    char scratch[256];
    size_t n;
    int heap;
    char *s = aw_format_tmp(fmt, ap, legacy, scratch, sizeof scratch, &n, &heap);
    if (!s)
        return -1;
    int r = store_bounded(buf, cap, s, n, c99);
    if (heap)
        free(s);
    return r;
}

/* Copies an unbounded result into buf (no capacity is known for the destination). */
static int format_unbounded(char *buf, const char *fmt, char *ap, int legacy)
{
    char scratch[256];
    size_t n;
    int heap;
    char *s = aw_format_tmp(fmt, ap, legacy, scratch, sizeof scratch, &n, &heap);
    if (!s)
        return -1;
    memcpy(buf, s, n + 1);
    if (heap)
        free(s);
    return (int)n;
}

API(msvcrt, int, vsprintf, (char *buf, const char *fmt, char *ap))
{
    return format_unbounded(buf, fmt, ap, 1);
}

API(msvcrt, int, sprintf, (char *buf, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = __aw_msvcrt_vsprintf(buf, fmt, (char *)ap);
    __builtin_ms_va_end(ap);
    return r;
}

API(msvcrt, int, vsnprintf, (char *buf, size_t cap, const char *fmt, char *ap))
{
    return format_bounded(buf, cap, fmt, ap, 1, 1);
}

API(msvcrt, int, _vsnprintf, (char *buf, size_t cap, const char *fmt, char *ap))
{
    return format_bounded(buf, cap, fmt, ap, 1, 0);
}

API(msvcrt, int, snprintf, (char *buf, size_t cap, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = __aw_msvcrt_vsnprintf(buf, cap, fmt, (char *)ap);
    __builtin_ms_va_end(ap);
    return r;
}

API(msvcrt, int, _snprintf, (char *buf, size_t cap, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = __aw_msvcrt__vsnprintf(buf, cap, fmt, (char *)ap);
    __builtin_ms_va_end(ap);
    return r;
}

/* ucrt: __stdio_common_vsprintf(options, buffer, buffer_count, format, locale, va_list).
 * buffer_count is the size of the buffer (C99 truncation); (size_t)-1 means unbounded. */
API(msvcrt, int, __stdio_common_vsprintf, (uint64_t options, char *buf, size_t count, const char *fmt, void *locale,
                                           char *ap))
{
    (void)locale;
    int legacy = (options & 0x18) != 0;
    if (count == (size_t)-1)
        return format_unbounded(buf, fmt, ap, legacy);
    return format_bounded(buf, count, fmt, ap, legacy, 1);
}

/* ucrt: __stdio_common_vsnprintf_s(options, buffer, size, max_count, format, locale, va_list).
 * max_count is the most characters to write (_TRUNCATE = size - 1). Without the C99 option
 * (0x2) a truncated result returns -1, as the secure CRT does. */
API(msvcrt, int, __stdio_common_vsnprintf_s, (uint64_t options, char *buf, size_t size, size_t max_count,
                                              const char *fmt, void *locale, char *ap))
{
    (void)locale;
    char scratch[256];
    size_t n;
    int heap;
    char *s = aw_format_tmp(fmt, ap, (options & 0x18) != 0, scratch, sizeof scratch, &n, &heap);
    if (!s)
        return -1;
    int r = -1;
    if (!buf || !size) {
        if (heap)
            free(s);
        return -1;
    }
    size_t lim = size - 1;
    if (max_count != (size_t)-1 && max_count < lim)
        lim = max_count;
    size_t k = n < lim ? n : lim;
    memcpy(buf, s, k);
    buf[k] = 0;
    if (n <= lim || (options & 0x2))
        r = (int)n;
    if (heap)
        free(s);
    return r;
}
