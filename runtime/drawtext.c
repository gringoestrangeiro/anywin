/* anywin text output (S4): TextOut/ExtTextOut, extents and metrics, and DrawText.
 *
 * Glyph coverage from font.c is alpha-blended with the text colour over the destination.
 * ANSI (…A) strings are UTF-8 (code page 65001); invalid UTF-8 bytes decode as Latin-1.
 * Text is laid out in device pixels: the DC origin mapping is applied to the anchor point. */
#include "gdi_int.h"

#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_BOTTOM 0x0008
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_NOCLIP 0x0100
#define DT_CALCRECT 0x0400
#define DT_NOPREFIX 0x0800
#define DT_EDITCONTROL 0x2000
#define DT_PATH_ELLIPSIS 0x4000
#define DT_END_ELLIPSIS 0x8000
#define DT_WORD_ELLIPSIS 0x40000
#define DT_HIDEPREFIX 0x100000

#define ETO_OPAQUE 0x0002
#define ETO_CLIPPED 0x0004

/* ---------------------------------------------------------------------- */
/* decoding                                                                */
/* ---------------------------------------------------------------------- */

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static uint32_t *cps_utf8(const char *s, int len, int *out_n)
{
    if (len < 0)
        len = s ? (int)strlen(s) : 0;
    uint32_t *a = malloc(sizeof *a * (size_t)(len + 1));
    if (!a)
        aw_fatal("gdi: out of memory");
    int n = 0;
    for (int i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int k = 1;
        if (c >= 0x80 && (c & 0xE0) == 0xC0 && i + 1 < len && is_cont((unsigned char)s[i + 1])) {
            cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            k = 2;
        } else if (c >= 0x80 && (c & 0xF0) == 0xE0 && i + 2 < len && is_cont((unsigned char)s[i + 1]) &&
                   is_cont((unsigned char)s[i + 2])) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 6) |
                 ((unsigned char)s[i + 2] & 0x3F);
            k = 3;
        } else if (c >= 0x80 && (c & 0xF8) == 0xF0 && i + 3 < len && is_cont((unsigned char)s[i + 1]) &&
                   is_cont((unsigned char)s[i + 2]) && is_cont((unsigned char)s[i + 3])) {
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 12) |
                 ((uint32_t)((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
            k = 4;
        }
        a[n++] = cp;
        i += k;
    }
    *out_n = n;
    return a;
}

static uint32_t *cps_utf16(const WCHAR *s, int len, int *out_n)
{
    if (len < 0)
        len = s ? (int)aw_wcslen(s) : 0;
    uint32_t *a = malloc(sizeof *a * (size_t)(len + 1));
    if (!a)
        aw_fatal("gdi: out of memory");
    int n = 0;
    for (int i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c < 0xE000) {
            c = 0xFFFD;
        }
        a[n++] = c;
    }
    *out_n = n;
    return a;
}

/* ---------------------------------------------------------------------- */
/* glyph runs                                                              */
/* ---------------------------------------------------------------------- */

static aw_font *dc_font(const gdi_dc *d)
{
    gdi_obj *f = gdi_get(d->font, GDI_OBJ_FONT);
    return f ? f->u.font : gdi_default_font();
}

static int text_width(aw_font *f, const uint32_t *cp, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w += font_advance(f, cp[i]);
    return w;
}

/* opaque background: device rectangle painted with the DC background colour */
static void fill_dev_text(gdi_dc *d, RECT r) { gdi_fill_dev(d, r, gdi_px(d->bk_color), R2_COPYPEN); }

static int in_extra(const RECT *x, int px, int py)
{
    return !x || (px >= x->left && px < x->right && py >= x->top && py < x->bottom);
}

/* horizontal line, device pixels, through the clip and the optional extra clip */
static void hline_dev(gdi_dc *d, int x0, int x1, int y, uint32_t px, const RECT *extra)
{
    for (int x = x0; x < x1; x++)
        if (in_extra(extra, x, y))
            gdi_alpha_put(d, x, y, px, 255);
    RECT r = {x0, y, x1, y + 1};
    gdi_mark(d, &r);
}

/* draws the codepoints with pen start x and baseline y (device).  dx: optional advance after
 * each character (device units); ul: optional underline flags per character.  Returns the
 * advance of the run. */
static int text_run(gdi_dc *d, aw_font *f, const uint32_t *cp, int n, const int *dx, const unsigned char *ul,
                    int x, int base, uint32_t tpx, const RECT *extra)
{
    int pen = x;
    for (int i = 0; i < n; i++) {
        const aw_glyph *g = font_glyph(f, cp[i]);
        int adv = dx ? dx[i] : g->advance;
        if (g->alpha && g->w > 0 && g->h > 0) {
            int gx = pen + g->left, gy = base - g->top;
            for (int j = 0; j < g->h; j++) {
                int py = gy + j;
                if (extra && (py < extra->top || py >= extra->bottom))
                    continue;
                for (int k = 0; k < g->w; k++) {
                    uint32_t a = g->alpha[j * g->w + k];
                    int px = gx + k;
                    if (!a || !in_extra(extra, px, py))
                        continue;
                    gdi_alpha_put(d, px, py, tpx, a);
                }
            }
            RECT box = {gx, gy, gx + g->w, gy + g->h};
            gdi_mark(d, &box);
        }
        if (ul && ul[i]) {
            const aw_font_metrics *m = font_metrics(f);
            int uy = base + (m->descent > 2 ? m->descent / 2 : 1);
            hline_dev(d, pen, pen + (adv > 0 ? adv : 1), uy, tpx, extra);
        }
        pen += adv;
    }
    return pen - x;
}

/* TextOut / ExtTextOut core: anchor (logical), alignment, optional opaque cell rectangle and
 * clip rectangle (logical).  Updates the current position for TA_UPDATECP. */
static void text_out_core(gdi_dc *d, int lx, int ly, const uint32_t *cp, int n, const int *dx,
                          const RECT *opaque, const RECT *clip)
{
    aw_font *f = dc_font(d);
    const aw_font_metrics *m = font_metrics(f);
    int align = d->text_align;
    if (align & TA_UPDATECP) {
        lx = d->cur_x;
        ly = d->cur_y;
    }
    int width = 0;
    if (dx)
        for (int i = 0; i < n; i++)
            width += dx[i];
    else
        width = text_width(f, cp, n);

    int start = lx;
    if ((align & 6) == TA_RIGHT)
        start = lx - width;
    else if ((align & 6) == TA_CENTER)
        start = lx - width / 2;
    int base_l = (align & 0x18) == TA_BOTTOM ? ly - m->descent
                 : (align & 0x18) == TA_BASELINE ? ly
                                                 : ly + m->ascent;

    int sx, sy;
    gdi_map(d, start, base_l, &sx, &sy);
    RECT dclip, *pclip = NULL;
    if (clip) {
        gdi_map_rect(d, clip, &dclip);
        pclip = &dclip;
    }
    if (opaque) {
        RECT o;
        gdi_map_rect(d, opaque, &o);
        fill_dev_text(d, o);
    } else if (d->bk_mode == OPAQUE && n > 0) {
        RECT cell = {sx, sy - m->ascent, sx + width, sy - m->ascent + m->height};
        fill_dev_text(d, cell);
    }
    uint32_t tpx = gdi_px(d->text_color);
    text_run(d, f, cp, n, dx, NULL, sx, sy, tpx, pclip);

    if (m->underline || m->strikeout) {
        int x0 = sx, x1 = sx + width;
        if (m->underline)
            hline_dev(d, x0, x1, sy + (m->descent > 2 ? m->descent / 2 : 1), tpx, pclip);
        if (m->strikeout)
            hline_dev(d, x0, x1, sy - m->ascent / 3, tpx, pclip);
    }
    if (align & TA_UPDATECP) {
        d->cur_x = start + width;
        d->cur_y = ly;
    }
}

/* ---------------------------------------------------------------------- */
/* public API: TextOut family, extents, metrics                             */
/* ---------------------------------------------------------------------- */

static int text_dc_check(HDC hdc, gdi_dc **out)
{
    *out = gdi_get_dc(hdc);
    if (!*out) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

API(gdi32, BOOL, TextOutA, (HDC hdc, int x, int y, LPCSTR s, int c))
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d))
        return FALSE;
    int n;
    uint32_t *cp = cps_utf8(s, c, &n);
    text_out_core(d, x, y, cp, n, NULL, NULL, NULL);
    free(cp);
    return TRUE;
}

API(gdi32, BOOL, TextOutW, (HDC hdc, int x, int y, LPCWSTR s, int c))
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d))
        return FALSE;
    int n;
    uint32_t *cp = cps_utf16(s, c, &n);
    text_out_core(d, x, y, cp, n, NULL, NULL, NULL);
    free(cp);
    return TRUE;
}

API(gdi32, BOOL, ExtTextOutW, (HDC hdc, int x, int y, UINT options, const RECT *rc, LPCWSTR s, UINT c, const INT *dx))
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d))
        return FALSE;
    int n;
    uint32_t *cp = cps_utf16(s, (int)c, &n);
    int *adv = NULL;
    if (dx && n > 0) {
        adv = malloc(sizeof *adv * (size_t)n);
        if (!adv)
            aw_fatal("gdi: out of memory");
        for (int i = 0; i < n; i++)
            adv[i] = dx[i];
    }
    RECT clip = {0, 0, 0, 0};
    if (rc)
        clip = *rc;
    text_out_core(d, x, y, cp, n, adv, (options & ETO_OPAQUE) && rc ? rc : NULL,
                  (options & ETO_CLIPPED) && rc ? &clip : NULL);
    free(adv);
    free(cp);
    return TRUE;
}

static BOOL extent(HDC hdc, const uint32_t *cp, int n, SIZE *sz)
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d))
        return FALSE;
    aw_font *f = dc_font(d);
    sz->cx = text_width(f, cp, n);
    sz->cy = font_metrics(f)->height;
    return TRUE;
}

API(gdi32, BOOL, GetTextExtentPoint32A, (HDC hdc, LPCSTR s, int c, SIZE *sz))
{
    int n;
    uint32_t *cp = cps_utf8(s, c, &n);
    BOOL ok = extent(hdc, cp, n, sz);
    free(cp);
    return ok;
}

API(gdi32, BOOL, GetTextExtentPoint32W, (HDC hdc, LPCWSTR s, int c, SIZE *sz))
{
    int n;
    uint32_t *cp = cps_utf16(s, c, &n);
    BOOL ok = extent(hdc, cp, n, sz);
    free(cp);
    return ok;
}

static void fill_tm_common(const aw_font_metrics *m, LONG *h, LONG *asc, LONG *desc, LONG *il, LONG *el,
                           LONG *avg, LONG *max, LONG *weight, LONG *dax, LONG *day, BYTE *italic,
                           BYTE *under, BYTE *strike, BYTE *pitch, BYTE *charset)
{
    *h = m->height;
    *asc = m->ascent;
    *desc = m->descent;
    *il = m->internal_leading;
    *el = m->external_leading;
    *avg = m->ave_char_width;
    *max = m->max_char_width;
    *weight = m->weight;
    *dax = 96;
    *day = 96;
    *italic = (BYTE)(m->italic != 0);
    *under = (BYTE)(m->underline != 0);
    *strike = (BYTE)(m->strikeout != 0);
    /* TMPF_FIXED_PITCH is set for variable pitch; TMPF_TRUETYPE 0x04; family FF_MODERN / FF_SWISS */
    *pitch = (BYTE)(m->fixed_pitch ? (0x30 | 0x04) : (0x20 | 0x01 | 0x04));
    *charset = 1; /* DEFAULT_CHARSET */
}

API(gdi32, BOOL, GetTextMetricsA, (HDC hdc, aw_textmetricA *tm))
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d) || !tm)
        return FALSE;
    const aw_font_metrics *m = font_metrics(dc_font(d));
    fill_tm_common(m, &tm->tmHeight, &tm->tmAscent, &tm->tmDescent, &tm->tmInternalLeading,
                   &tm->tmExternalLeading, &tm->tmAveCharWidth, &tm->tmMaxCharWidth, &tm->tmWeight,
                   &tm->tmDigitizedAspectX, &tm->tmDigitizedAspectY, &tm->tmItalic, &tm->tmUnderlined,
                   &tm->tmStruckOut, &tm->tmPitchAndFamily, &tm->tmCharSet);
    tm->tmOverhang = 0;
    tm->tmFirstChar = 32;
    tm->tmLastChar = 255;
    tm->tmDefaultChar = '?';
    tm->tmBreakChar = ' ';
    return TRUE;
}

API(gdi32, BOOL, GetTextMetricsW, (HDC hdc, aw_textmetricW *tm))
{
    gdi_dc *d;
    if (!text_dc_check(hdc, &d) || !tm)
        return FALSE;
    const aw_font_metrics *m = font_metrics(dc_font(d));
    fill_tm_common(m, &tm->tmHeight, &tm->tmAscent, &tm->tmDescent, &tm->tmInternalLeading,
                   &tm->tmExternalLeading, &tm->tmAveCharWidth, &tm->tmMaxCharWidth, &tm->tmWeight,
                   &tm->tmDigitizedAspectX, &tm->tmDigitizedAspectY, &tm->tmItalic, &tm->tmUnderlined,
                   &tm->tmStruckOut, &tm->tmPitchAndFamily, &tm->tmCharSet);
    tm->tmOverhang = 0;
    tm->tmFirstChar = 32;
    tm->tmLastChar = 255;
    tm->tmDefaultChar = '?';
    tm->tmBreakChar = ' ';
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* DrawText                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t *cp;
    unsigned char *ul;
    int n;
    int w;
} dtline;

static void line_free(dtline *l)
{
    free(l->cp);
    free(l->ul);
}

static void line_push(dtline **ls, int *nl, int *cap, const uint32_t *cp, const unsigned char *ul, int n)
{
    if (*nl == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        dtline *p = realloc(*ls, sizeof **ls * (size_t)nc);
        if (!p)
            aw_fatal("gdi: out of memory");
        *ls = p;
        *cap = nc;
    }
    dtline *l = &(*ls)[(*nl)++];
    l->cp = malloc(sizeof *l->cp * (size_t)(n + 1));
    l->ul = malloc((size_t)n + 1);
    if (!l->cp || !l->ul)
        aw_fatal("gdi: out of memory");
    if (n)
        memcpy(l->cp, cp, sizeof *cp * (size_t)n);
    if (n)
        memcpy(l->ul, ul, (size_t)n);
    l->n = n;
    l->w = 0;
}

static int draw_text(gdi_dc *d, const uint32_t *in, int n, RECT *rc, UINT flags)
{
    aw_font *f = dc_font(d);
    const aw_font_metrics *m = font_metrics(f);
    int lh = m->height;
    int single = (flags & DT_SINGLELINE) != 0;
    int wrap = !single && (flags & DT_WORDBREAK) != 0;
    int ellipsis = (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) != 0;
    int maxw = rc->right - rc->left;

    /* 1. mnemonics ('&' underlines the next character), CR/LF, tabs (up to 8 cells each) */
    uint32_t *cp = malloc(sizeof *cp * (size_t)(n * 8 + 2));
    unsigned char *ul = malloc((size_t)(n * 8 + 2));
    if (!cp || !ul)
        aw_fatal("gdi: out of memory");
    int m_n = 0, pending = 0, col = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = in[i];
        if (c == '&' && !(flags & DT_NOPREFIX)) {
            if (i + 1 < n && in[i + 1] == '&') {
                cp[m_n] = '&';
                ul[m_n++] = 0;
                col++;
                i++;
            } else if (i + 1 < n) {
                pending = (flags & DT_HIDEPREFIX) == 0;
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < n && in[i + 1] == '\n')
                i++;
            cp[m_n] = single ? ' ' : 0x0A;
            ul[m_n++] = 0;
            col = 0;
            continue;
        }
        if (c == '\t') {
            if (flags & DT_EXPANDTABS) {
                int spaces = 8 - (col % 8);
                for (int s = 0; s < spaces; s++) {
                    cp[m_n] = ' ';
                    ul[m_n++] = 0;
                    col++;
                }
            } else {
                cp[m_n] = ' ';
                ul[m_n++] = 0;
                col++;
            }
            continue;
        }
        cp[m_n] = c;
        ul[m_n++] = (unsigned char)pending;
        pending = 0;
        col++;
    }

    /* 2. split into lines at the 0x0A sentinels, wrapping when requested */
    dtline *lines = NULL;
    int nl = 0, cap = 0;
    int s = 0;
    for (int i = 0; i <= m_n; i++) {
        if (i < m_n && cp[i] != 0x0A)
            continue;
        int e = i;
        if (!wrap || maxw <= 0) {
            line_push(&lines, &nl, &cap, cp + s, ul + s, e - s);
        } else {
            int start = s;
            while (start < e) {
                int w = 0, last_space = -1, k = start;
                for (k = start; k < e; k++) {
                    int cw = font_advance(f, cp[k]);
                    if (w + cw > maxw && k > start)
                        break;
                    w += cw;
                    if (cp[k] == ' ')
                        last_space = k;
                }
                int end = k;
                if (end < e && last_space >= start)
                    end = last_space + 1;
                line_push(&lines, &nl, &cap, cp + start, ul + start, end - start);
                start = end;
                while (start < e && cp[start] == ' ')
                    start++;
            }
            if (s == e)
                line_push(&lines, &nl, &cap, cp + s, ul + s, 0);
        }
        s = i + 1;
    }

    /* 3. measure, truncate with an ellipsis when asked, compute the block size */
    int block_w = 0;
    for (int i = 0; i < nl; i++) {
        dtline *l = &lines[i];
        l->w = text_width(f, l->cp, l->n);
        if (ellipsis && !(flags & DT_CALCRECT) && l->w > maxw && maxw > 0) {
            int dots = font_advance(f, '.') * 3;
            int keep = l->n;
            while (keep > 0 && text_width(f, l->cp, keep) + dots > maxw)
                keep--;
            uint32_t *nc = malloc(sizeof *nc * (size_t)(keep + 3));
            unsigned char *nu = malloc((size_t)keep + 3);
            if (!nc || !nu)
                aw_fatal("gdi: out of memory");
            memcpy(nc, l->cp, sizeof *nc * (size_t)keep);
            memcpy(nu, l->ul, (size_t)keep);
            for (int k = 0; k < 3; k++) {
                nc[keep + k] = '.';
                nu[keep + k] = 0;
            }
            free(l->cp);
            free(l->ul);
            l->cp = nc;
            l->ul = nu;
            l->n = keep + 3;
            l->w = text_width(f, l->cp, l->n);
        }
        if (l->w > block_w)
            block_w = l->w;
    }
    int block_h = nl * lh;
    if (flags & DT_CALCRECT) {
        rc->right = rc->left + (wrap || ellipsis ? (block_w < maxw ? block_w : maxw) : block_w);
        rc->bottom = rc->top + block_h;
    } else {
        /* 4. place and draw */
        RECT dr;
        gdi_map_rect(d, rc, &dr);
        int top = dr.top;
        if (single && (flags & DT_BOTTOM))
            top = dr.bottom - block_h;
        else if (single && (flags & DT_VCENTER))
            top = dr.top + (dr.bottom - dr.top - block_h) / 2;
        RECT extra = dr;
        const RECT *pextra = (flags & DT_NOCLIP) ? NULL : &extra;
        uint32_t tpx = gdi_px(d->text_color);
        int rw = dr.right - dr.left;
        for (int i = 0; i < nl; i++) {
            dtline *l = &lines[i];
            int x = dr.left;
            if ((flags & 3) == DT_RIGHT)
                x = dr.right - l->w;
            else if ((flags & 3) == DT_CENTER)
                x = dr.left + (rw - l->w) / 2;
            int y = top + i * lh;
            if (d->bk_mode == OPAQUE && l->w > 0) {
                RECT cell = {x, y, x + l->w, y + lh};
                fill_dev_text(d, cell);
            }
            text_run(d, f, l->cp, l->n, NULL, l->ul, x, y + m->ascent, tpx, pextra);
        }
    }
    for (int i = 0; i < nl; i++)
        line_free(&lines[i]);
    free(lines);
    free(cp);
    free(ul);
    return block_h;
}

static int draw_text_entry(HDC hdc, const uint32_t *cp, int n, RECT *rc, UINT flags)
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    return draw_text(d, cp, n, rc, flags);
}

API(user32, int, DrawTextA, (HDC hdc, LPCSTR s, int c, RECT *rc, UINT flags))
{
    int n;
    uint32_t *cp = cps_utf8(s, c, &n);
    int h = draw_text_entry(hdc, cp, n, rc, flags);
    free(cp);
    return h;
}

API(user32, int, DrawTextW, (HDC hdc, LPCWSTR s, int c, RECT *rc, UINT flags))
{
    int n;
    uint32_t *cp = cps_utf16(s, c, &n);
    int h = draw_text_entry(hdc, cp, n, rc, flags);
    free(cp);
    return h;
}
