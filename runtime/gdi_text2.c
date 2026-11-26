/* anywin GDI text services (S-GDI2): font creation from LOGFONT, per-character widths and
 * placement, outline metrics, ANSI extent/ExtTextOut wrappers and charset translation.
 *
 * ANSI (...A) strings are UTF-8 in this runtime (see drawtext.c).  Single-byte character codes
 * passed to GetCharWidth* are Latin-1 code points, the same fallback used for invalid UTF-8. */
#include "gdi_int.h"
#include "user32_int.h"
#include "aw_api.h"

#define AW_DEFAULT_CHARSET 1
#define AW_ERROR_INVALID_PARAMETER_87 87

typedef struct { float abcfA, abcfB, abcfC; } aw_abcfloat;

typedef struct {
    UINT otmSize;
    aw_textmetricA otmTextMetrics;
    BYTE otmFiller;
    BYTE otmPanoseNumber[10];
    UINT otmfsSelection, otmfsType;
    int otmsCharSlopeRise, otmsCharSlopeRun, otmItalicAngle;
    UINT otmEMSquare;
    int otmAscent, otmDescent;
    UINT otmLineGap, otmsCapEmHeight, otmsXHeight;
    RECT otmrcFontBox;
    int otmMacAscent, otmMacDescent;
    UINT otmMacLineGap, otmusMinimumPPEM;
    POINT otmptSubscriptSize, otmptSubscriptOffset, otmptSuperscriptSize, otmptSuperscriptOffset;
    UINT otmsStrikeoutSize;
    int otmsStrikeoutPosition, otmsUnderscoreSize, otmsUnderscorePosition;
    char *otmpFamilyName, *otmpFaceName, *otmpStyleName, *otmpFullName;
} aw_outline_tm_a;

typedef struct {
    UINT lStructSize;
    WCHAR *lpOutString;
    UINT *lpOrder;
    int *lpDx;
    int *lpCaretPos;
    WCHAR *lpClass;
    WCHAR *lpGlyphs;
    UINT nGlyphs, nMaxFit;
} aw_gcp_results_w;

typedef struct {
    UINT ciCharset, ciACP;
    DWORD fsUsb[4], fsCsb[2];
} aw_charsetinfo;

static aw_font *dc_font_of(HDC hdc)
{
    gdi_dc *d = gdi_get_dc(hdc);
    return d ? gdi_dc_font(hdc) : NULL;
}

/* advance of one character code (code point) in the DC's font */
static int adv_of(aw_font *f, uint32_t cp) { return font_advance(f, cp); }

/* UTF-16 code units of an ANSI (UTF-8) string: malloc'd, *n units */
static WCHAR *ansi_units(const char *s, int len, int *n)
{
    if (len < 0)
        len = s ? (int)strlen(s) : 0;
    WCHAR *w = malloc(sizeof *w * (size_t)(len + 1));
    if (!w)
        aw_fatal("gdi: out of memory");
    *n = aw_u2w_buf(s ? s : "", len, w, len + 1);
    if (*n < 0)
        *n = 0;
    return w;
}

/* code point of the UTF-16 unit at i (combining a surrogate pair); *k = units consumed */
static uint32_t cp_at(const WCHAR *w, int n, int i, int *k)
{
    uint32_t c = w[i];
    *k = 1;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
        *k = 2;
        return 0x10000u + ((c - 0xD800u) << 10) + (w[i + 1] - 0xDC00u);
    }
    return c;
}

/* sum of advances of the n units of w in f */
static int units_width(aw_font *f, const WCHAR *w, int n)
{
    int tot = 0, k;
    for (int i = 0; i < n; i += k)
        tot += adv_of(f, cp_at(w, n, i, &k));
    return tot;
}

/* ---------------------------------------------------------------------- */
/* fonts                                                                   */
/* ---------------------------------------------------------------------- */

API(gdi32, HFONT, CreateFontIndirectA, (const LOGFONTA *lf))
{
    if (!lf) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return NULL;
    }
    char face[33];
    memcpy(face, lf->lfFaceName, 32);
    face[32] = 0;
    return __aw_gdi32_CreateFontA(lf->lfHeight, lf->lfWidth, lf->lfEscapement, lf->lfOrientation, lf->lfWeight,
                                  lf->lfItalic, lf->lfUnderline, lf->lfStrikeOut, lf->lfCharSet,
                                  lf->lfOutPrecision, lf->lfClipPrecision, lf->lfQuality,
                                  lf->lfPitchAndFamily, face);
}

API(gdi32, HFONT, CreateFontIndirectW, (const LOGFONTW *lf))
{
    if (!lf) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return NULL;
    }
    return __aw_gdi32_CreateFontW(lf->lfHeight, lf->lfWidth, lf->lfEscapement, lf->lfOrientation, lf->lfWeight,
                                  lf->lfItalic, lf->lfUnderline, lf->lfStrikeOut, lf->lfCharSet,
                                  lf->lfOutPrecision, lf->lfClipPrecision, lf->lfQuality,
                                  lf->lfPitchAndFamily, lf->lfFaceName);
}

/* ---------------------------------------------------------------------- */
/* character widths                                                        */
/* ---------------------------------------------------------------------- */

static BOOL char_widths(HDC hdc, UINT first, UINT last, int *out)
{
    aw_font *f = dc_font_of(hdc);
    if (!f || !out || last < first) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    for (UINT c = first; c <= last; c++)
        out[c - first] = adv_of(f, c);
    return TRUE;
}

API(gdi32, BOOL, GetCharWidth32A, (HDC hdc, UINT first, UINT last, INT *buf))
{
    return char_widths(hdc, first, last, buf);
}

API(gdi32, BOOL, GetCharWidth32W, (HDC hdc, UINT first, UINT last, INT *buf))
{
    return char_widths(hdc, first, last, buf);
}

API(gdi32, BOOL, GetCharWidthA, (HDC hdc, UINT first, UINT last, INT *buf))
{
    return char_widths(hdc, first, last, buf);
}

API(gdi32, BOOL, GetCharWidthW, (HDC hdc, UINT first, UINT last, INT *buf))
{
    return char_widths(hdc, first, last, buf);
}

/* ABC widths as Windows reports them: A = left side bearing (pen to first ink), B = ink width,
 * C = advance - A - B.  Blank glyphs have A = B = 0 and C = advance. */
static void abc_of(aw_font *f, uint32_t cp, int *a, int *b, int *c)
{
    const aw_glyph *g = font_glyph(f, cp);
    int lsb = 0, ink = 0;
    if (g->alpha && g->w > 0 && g->h > 0) {
        lsb = g->left;
        ink = g->w;
    }
    *a = lsb;
    *b = ink;
    *c = g->advance - lsb - ink;
}

static BOOL abc_widths(HDC hdc, UINT first, UINT last, int *out)
{
    aw_font *f = dc_font_of(hdc);
    if (!f || !out || last < first) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    for (UINT c = first; c <= last; c++)
        abc_of(f, c, &out[3 * (c - first)], &out[3 * (c - first) + 1], &out[3 * (c - first) + 2]);
    return TRUE;
}

API(gdi32, BOOL, GetCharABCWidthsFloatA, (HDC hdc, UINT first, UINT last, aw_abcfloat *buf))
{
    if (!buf || last < first) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    int *tmp = malloc(sizeof *tmp * 3 * (size_t)(last - first + 1));
    if (!tmp)
        return FALSE;
    BOOL ok = abc_widths(hdc, first, last, tmp);
    for (UINT i = 0; ok && i <= last - first; i++) {
        buf[i].abcfA = (float)tmp[3 * i];
        buf[i].abcfB = (float)tmp[3 * i + 1];
        buf[i].abcfC = (float)tmp[3 * i + 2];
    }
    free(tmp);
    return ok;
}

typedef struct { int abcA, abcB, abcC; } aw_abc;

API(gdi32, BOOL, GetCharABCWidthsA, (HDC hdc, UINT first, UINT last, aw_abc *buf))
{
    if (!buf || last < first) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    return abc_widths(hdc, first, last, (int *)buf);
}

API(gdi32, BOOL, GetCharABCWidthsW, (HDC hdc, UINT first, UINT last, aw_abc *buf))
{
    return __aw_gdi32_GetCharABCWidthsA(hdc, first, last, buf);
}

/* ---------------------------------------------------------------------- */
/* extents                                                                 */
/* ---------------------------------------------------------------------- */

API(gdi32, BOOL, GetTextExtentPointA, (HDC hdc, LPCSTR s, int c, SIZE *sz))
{
    return __aw_gdi32_GetTextExtentPoint32A(hdc, s, c, sz);
}

API(gdi32, BOOL, GetTextExtentExPointA,
    (HDC hdc, LPCSTR s, int c, int max_ext, INT *fit, INT *dx, SIZE *sz))
{
    aw_font *f = dc_font_of(hdc);
    if (!f || !sz) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    int n;
    WCHAR *w = ansi_units(s, c, &n);
    int idx = 0, cum = 0, nfit = 0;
    for (int i = 0, k; i < n; i += k, idx++) {
        cum += adv_of(f, cp_at(w, n, i, &k));
        if (dx)
            dx[idx] = cum; /* alpDx holds partial string widths */
        if (max_ext <= 0 || cum <= max_ext)
            nfit = idx + 1; /* widths are monotonic, so the fitting characters form a prefix */
    }
    if (fit)
        *fit = nfit;
    sz->cx = units_width(f, w, n);
    sz->cy = font_metrics(f)->height;
    free(w);
    return TRUE;
}

API(gdi32, BOOL, ExtTextOutA,
    (HDC hdc, int x, int y, UINT options, const RECT *rc, LPCSTR s, UINT c, const INT *dx))
{
    int n;
    WCHAR *w = ansi_units(s, (int)c, &n);
    BOOL ok = __aw_gdi32_ExtTextOutW(hdc, x, y, options, rc, w, (UINT)n, dx);
    free(w);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* placement and outline metrics                                           */
/* ---------------------------------------------------------------------- */

API(gdi32, DWORD, GetCharacterPlacementW,
    (HDC hdc, LPCWSTR str, int count, int max_extent, aw_gcp_results_w *res, DWORD flags))
{
    (void)max_extent;
    (void)flags;
    aw_font *f = dc_font_of(hdc);
    if (!f || !str || count < 0)
        return 0;
    int n = count;
    int width = units_width(f, str, n);
    int nglyphs = 0;
    for (int i = 0, k; i < n; i += k, nglyphs++)
        cp_at(str, n, i, &k);
    if (res && res->lStructSize >= sizeof *res) {
        if (res->lpOutString)
            memcpy(res->lpOutString, str, sizeof(WCHAR) * (size_t)n);
        for (int i = 0, k; i < n; i += k) {
            int a = adv_of(f, cp_at(str, n, i, &k));
            if (res->lpDx)
                res->lpDx[i] = a;
            if (res->lpOrder)
                res->lpOrder[i] = (UINT)i;
        }
        res->nGlyphs = (UINT)nglyphs;
        res->nMaxFit = (UINT)nglyphs;
    }
    return (DWORD)(uint16_t)width | ((DWORD)(uint16_t)font_metrics(f)->height << 16);
}

API(gdi32, UINT, GetOutlineTextMetricsA, (HDC hdc, UINT cb, aw_outline_tm_a *otm))
{
    aw_font *f = dc_font_of(hdc);
    if (!f)
        return 0;
    if (!otm)
        return sizeof *otm;
    if (cb < sizeof *otm)
        return 0;
    const aw_font_metrics *m = font_metrics(f);
    memset(otm, 0, sizeof *otm);
    otm->otmSize = sizeof *otm;
    aw_textmetricA *t = &otm->otmTextMetrics;
    t->tmHeight = m->height;
    t->tmAscent = m->ascent;
    t->tmDescent = m->descent;
    t->tmInternalLeading = m->internal_leading;
    t->tmExternalLeading = m->external_leading;
    t->tmAveCharWidth = m->ave_char_width;
    t->tmMaxCharWidth = m->max_char_width;
    t->tmWeight = m->weight;
    t->tmDigitizedAspectX = t->tmDigitizedAspectY = 96;
    t->tmItalic = (BYTE)(m->italic != 0);
    t->tmUnderlined = (BYTE)(m->underline != 0);
    t->tmStruckOut = (BYTE)(m->strikeout != 0);
    t->tmPitchAndFamily = (BYTE)(m->fixed_pitch ? 0x30 | 0x04 : 0x20 | 0x01 | 0x04);
    t->tmCharSet = 0; /* ANSI_CHARSET */
    t->tmFirstChar = 32;
    t->tmLastChar = 255;
    t->tmDefaultChar = '?';
    t->tmBreakChar = ' ';
    otm->otmfsSelection = 0;
    otm->otmfsType = 0x0001 /* TrueType-like, embeddable */;
    otm->otmEMSquare = (UINT)(m->height > 0 ? m->height : 1);
    otm->otmAscent = m->ascent;
    otm->otmDescent = -m->descent;
    otm->otmLineGap = (UINT)(m->external_leading > 0 ? m->external_leading : 0);
    otm->otmsCapEmHeight = (UINT)m->ascent;
    otm->otmsXHeight = (UINT)(m->ascent * 2 / 3);
    otm->otmrcFontBox.left = 0;
    otm->otmrcFontBox.top = m->ascent;
    otm->otmrcFontBox.right = m->max_char_width;
    otm->otmrcFontBox.bottom = -m->descent;
    otm->otmMacAscent = m->ascent;
    otm->otmMacDescent = -m->descent;
    otm->otmMacLineGap = otm->otmLineGap;
    otm->otmusMinimumPPEM = (UINT)m->height;
    otm->otmsStrikeoutSize = 1;
    otm->otmsStrikeoutPosition = m->ascent / 3;
    otm->otmsUnderscoreSize = 1;
    otm->otmsUnderscorePosition = -(m->descent / 2 > 1 ? m->descent / 2 : 1);
    return sizeof *otm;
}

/* ---------------------------------------------------------------------- */
/* charsets                                                                */
/* ---------------------------------------------------------------------- */

/* charset -> code page (the common Windows pairs); 0 = unknown */
static UINT charset_to_cp(UINT cs)
{
    switch (cs) {
    case 0: return 1252;    /* ANSI_CHARSET */
    case 128: return 932;   /* SHIFTJIS */
    case 129: return 949;   /* HANGEUL */
    case 134: return 936;   /* GB2312 */
    case 136: return 950;   /* CHINESEBIG5 */
    case 161: return 1253;  /* GREEK */
    case 162: return 1254;  /* TURKISH */
    case 177: return 1255;  /* HEBREW */
    case 178: return 1256;  /* ARABIC */
    case 186: return 1257;  /* BALTIC */
    case 204: return 1251;  /* RUSSIAN */
    case 222: return 874;   /* THAI */
    case 238: return 1250;  /* EASTEUROPE */
    case 255: return 437;   /* OEM_CHARSET */
    default: return 0;
    }
}

static UINT cp_to_charset(UINT cp)
{
    switch (cp) {
    case 1252: return 0;
    case 932: return 128;
    case 949: return 129;
    case 936: return 134;
    case 950: return 136;
    case 1253: return 161;
    case 1254: return 162;
    case 1255: return 177;
    case 1256: return 178;
    case 1257: return 186;
    case 1251: return 204;
    case 874: return 222;
    case 1250: return 238;
    case 437: return 255;
    default: return 0;
    }
}

API(gdi32, BOOL, TranslateCharsetInfo, (DWORD *src, aw_charsetinfo *cs, DWORD options))
{
    if (!cs) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    UINT charset = 0, cp = 0;
    switch (options) {
    case 1: /* TCI_SRCCHARSET: the charset is passed by value in lpSrc */
        charset = (UINT)(uintptr_t)src;
        cp = charset_to_cp(charset);
        break;
    case 2: /* TCI_SRCCODEPAGE: the code page is passed by value in lpSrc */
        cp = (UINT)(uintptr_t)src;
        charset = cp_to_charset(cp);
        break;
    case 0: /* TCI_NOTIMPLEMENTED: Windows reports nothing useful; the ANSI charset is returned */
    case 3: /* TCI_SRCFONTSIG: only Latin 1 font signatures are recognised */
        charset = 0;
        cp = 1252;
        break;
    default:
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    if (!cp) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER_87);
        return FALSE;
    }
    memset(cs, 0, sizeof *cs);
    cs->ciCharset = charset;
    cs->ciACP = cp;
    cs->fsCsb[0] = 0x1; /* FONTSIGNATURE: Latin 1 */
    cs->fsUsb[0] = 0x1;
    return TRUE;
}
