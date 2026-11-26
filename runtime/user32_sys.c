/* user32 core - system metrics, system colours, DPI (fixed at 96) (S5). */
#include "user32_int.h"

#define AW_RGBC(r, g, b) AW_RGB(r, g, b)

static const COLORREF g_syscolors[31] = {
    AW_RGBC(200, 200, 200), /* 0 SCROLLBAR */
    AW_RGBC(0, 0, 0),       /* 1 DESKTOP */
    AW_RGBC(0, 120, 215),   /* 2 ACTIVECAPTION */
    AW_RGBC(153, 153, 153), /* 3 INACTIVECAPTION */
    AW_RGBC(240, 240, 240), /* 4 MENU */
    AW_RGBC(255, 255, 255), /* 5 WINDOW */
    AW_RGBC(100, 100, 100), /* 6 WINDOWFRAME */
    AW_RGBC(0, 0, 0),       /* 7 MENUTEXT */
    AW_RGBC(0, 0, 0),       /* 8 WINDOWTEXT */
    AW_RGBC(255, 255, 255), /* 9 CAPTIONTEXT */
    AW_RGBC(180, 180, 180), /* 10 ACTIVEBORDER */
    AW_RGBC(244, 247, 252), /* 11 INACTIVEBORDER */
    AW_RGBC(171, 171, 171), /* 12 APPWORKSPACE */
    AW_RGBC(0, 120, 215),   /* 13 HIGHLIGHT */
    AW_RGBC(255, 255, 255), /* 14 HIGHLIGHTTEXT */
    AW_RGBC(240, 240, 240), /* 15 BTNFACE */
    AW_RGBC(160, 160, 160), /* 16 BTNSHADOW */
    AW_RGBC(109, 109, 109), /* 17 GRAYTEXT */
    AW_RGBC(0, 0, 0),       /* 18 BTNTEXT */
    AW_RGBC(0, 0, 0),       /* 19 INACTIVECAPTIONTEXT */
    AW_RGBC(255, 255, 255), /* 20 BTNHIGHLIGHT */
    AW_RGBC(105, 105, 105), /* 21 3DDKSHADOW */
    AW_RGBC(227, 227, 227), /* 22 3DLIGHT */
    AW_RGBC(0, 0, 0),       /* 23 INFOTEXT */
    AW_RGBC(255, 255, 225), /* 24 INFOBK */
    AW_RGBC(0, 0, 0),       /* 25 unused */
    AW_RGBC(0, 102, 204),   /* 26 HOTLIGHT */
    AW_RGBC(185, 209, 234), /* 27 GRADIENTACTIVECAPTION */
    AW_RGBC(215, 228, 242), /* 28 GRADIENTINACTIVECAPTION */
    AW_RGBC(0, 120, 215),   /* 29 MENUHILIGHT */
    AW_RGBC(240, 240, 240), /* 30 MENUBAR */
};

COLORREF aw_syscolor(int idx)
{
    if (idx < 0 || idx > 30)
        return 0;
    return g_syscolors[idx];
}

API(user32, COLORREF, GetSysColor, (int idx)) { return aw_syscolor(idx); }

static HBRUSH g_sysbrush[31];
API(user32, HBRUSH, GetSysColorBrush, (int idx))
{
    if (idx < 0 || idx > 30)
        return NULL;
    if (!g_sysbrush[idx])
        g_sysbrush[idx] = __aw_gdi32_CreateSolidBrush(aw_syscolor(idx));
    return g_sysbrush[idx];
}

int aw_system_metric(int idx)
{
    int sw = 1024, sh = 768;
    if (aw_display)
        plat_screen_size(&sw, &sh);
    switch (idx) {
    case SM_CXSCREEN: return sw;
    case SM_CYSCREEN: return sh;
    case SM_CXVSCROLL: return 17;
    case SM_CYHSCROLL: return 17;
    case SM_CYCAPTION: return 23;
    case SM_CXBORDER: return 1;
    case SM_CYBORDER: return 1;
    case SM_CXDLGFRAME: return 4;
    case SM_CYDLGFRAME: return 4;
    case SM_CYVTHUMB: return 17;
    case SM_CXHTHUMB: return 17;
    case SM_CXICON: return 32;
    case SM_CYICON: return 32;
    case SM_CXCURSOR: return 32;
    case SM_CYCURSOR: return 32;
    case SM_CYMENU: return 20;
    case SM_CXFULLSCREEN: return sw;
    case SM_CYFULLSCREEN: return sh - 23 - 20;
    case SM_MOUSEPRESENT: return 1;
    case SM_CYVSCROLL: return 17;
    case SM_CXHSCROLL: return 17;
    case SM_CXMIN: return 112;
    case SM_CYMIN: return 27;
    case SM_CXSIZE: return 18;
    case SM_CYSIZE: return 18;
    case SM_CXFRAME: return 4;
    case SM_CYFRAME: return 4;
    case SM_CXMINTRACK: return 112;
    case SM_CYMINTRACK: return 27;
    case SM_CXDOUBLECLK: return 4;
    case SM_CYDOUBLECLK: return 4;
    case SM_CXICONSPACING: return 75;
    case SM_CYICONSPACING: return 75;
    case SM_CMOUSEBUTTONS: return 3;
    case SM_CXEDGE: return 2;
    case SM_CYEDGE: return 2;
    case SM_CXMINSPACING: return 160;
    case SM_CYMINSPACING: return 24;
    case SM_CXSMICON: return 16;
    case SM_CYSMICON: return 16;
    case SM_CYSMCAPTION: return 15;
    case SM_CXSMSIZE: return 15;
    case SM_CYSMSIZE: return 15;
    case SM_CXMENUSIZE: return 18;
    case SM_CYMENUSIZE: return 18;
    case SM_CXMINIMIZED: return 160;
    case SM_CYMINIMIZED: return 24;
    case SM_CXMAXIMIZED: return sw;
    case SM_CYMAXIMIZED: return sh - 40;
    case SM_CXDRAG: return 4;
    case SM_CYDRAG: return 4;
    case SM_CXMENUCHECK: return 13;
    case SM_CYMENUCHECK: return 13;
    case SM_MOUSEWHEELPRESENT: return 1;
    case SM_CXVIRTUALSCREEN: return sw;
    case SM_CYVIRTUALSCREEN: return sh;
    case SM_CMONITORS: return 1;
    case SM_SAMEDISPLAYFORMAT: return 1;
    case SM_CXFOCUSBORDER: return 1;
    case SM_CYFOCUSBORDER: return 1;
    case SM_CARETBLINKINGENABLED: return 1;
    default: return 0;
    }
}

API(user32, int, GetSystemMetrics, (int idx)) { return aw_system_metric(idx); }
API(user32, int, GetSystemMetricsForDpi, (int idx, UINT dpi))
{
    (void)dpi;
    return aw_system_metric(idx);
}

API(user32, UINT, GetDpiForWindow, (HWND h)) { (void)h; return 96; }
API(user32, UINT, GetDpiForSystem, (void)) { return 96; }
API(user32, BOOL, SetProcessDPIAware, (void)) { return TRUE; }
API(user32, BOOL, IsProcessDPIAware, (void)) { return TRUE; }

/* ---- accelerator tables ------------------------------------------------- */
/* ACCEL is 6 bytes on Windows (fVirt, key, cmd) with no padding */
typedef struct __attribute__((packed)) { BYTE fVirt; WORD key; WORD cmd; } ACCEL_W;
struct accel_table { int count; ACCEL_W entries[]; };

API(user32, HACCEL, CreateAcceleratorTableW, (ACCEL_W * entries, int count))
{
    if (count <= 0 || !entries) {
        aw_set_last_error(87);
        return NULL;
    }
    struct accel_table *t = malloc(sizeof *t + (size_t)count * sizeof(ACCEL_W));
    if (!t) {
        aw_set_last_error(8);
        return NULL;
    }
    t->count = count;
    memcpy(t->entries, entries, (size_t)count * sizeof(ACCEL_W));
    aw_set_last_error(0);
    return (HACCEL)t;
}

API(user32, BOOL, DestroyAcceleratorTable, (HACCEL table))
{
    if (!table) {
        aw_set_last_error(6);
        return FALSE;
    }
    free(table);
    return TRUE;
}

/* Keyboard translation is not performed: the message always reaches the window procedure. */
API(user32, int, TranslateAcceleratorW, (HWND hwnd, HACCEL table, void *msg))
{
    (void)hwnd;
    (void)table;
    (void)msg;
    return 0;
}

/* ---- SystemParametersInfo ----------------------------------------------- */
/* LOGFONTW, x64 layout */
typedef struct {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONT_W;

/* NONCLIENTMETRICSW, x64 layout */
typedef struct {
    UINT cbSize;
    int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight;
    LOGFONT_W lfCaptionFont;
    int iSmCaptionWidth, iSmCaptionHeight;
    LOGFONT_W lfSmCaptionFont;
    int iMenuWidth, iMenuHeight;
    LOGFONT_W lfMenuFont, lfStatusFont, lfMessageFont;
    int iPaddedBorderWidth;
} NONCLIENTMETRICS_W;

/* ICONMETRICSW, x64 layout */
typedef struct {
    UINT cbSize;
    int iHorzSpacing, iVertSpacing, iTitleWrap;
    LOGFONT_W lfFont;
} ICONMETRICS_W;

static void default_font(LOGFONT_W *f, LONG height)
{
    memset(f, 0, sizeof *f);
    f->lfHeight = height;
    f->lfWeight = 400;
    f->lfCharSet = 1; /* DEFAULT_CHARSET */
    f->lfQuality = 5; /* CLEARTYPE_QUALITY */
    const char *face = "Segoe UI"; /* font.c maps this name to the sans family */
    for (size_t i = 0; face[i]; i++)
        f->lfFaceName[i] = (WCHAR)(unsigned char)face[i];
}

API(user32, BOOL, SystemParametersInfoW, (UINT action, UINT param, void *pv, UINT winini))
{
    (void)param;
    (void)winini;
    switch (action) {
    case 0x0030: /* SPI_GETWORKAREA */
        if (!pv)
            break;
        ((RECT *)pv)->left = 0;
        ((RECT *)pv)->top = 0;
        ((RECT *)pv)->right = __aw_user32_GetSystemMetrics(0);
        ((RECT *)pv)->bottom = __aw_user32_GetSystemMetrics(1);
        return TRUE;
    case 0x0029: { /* SPI_GETNONCLIENTMETRICS */
        NONCLIENTMETRICS_W *m = pv;
        if (!m || m->cbSize < sizeof *m)
            break;
        m->iBorderWidth = 1;
        m->iScrollWidth = m->iScrollHeight = 17;
        m->iCaptionWidth = m->iCaptionHeight = 19;
        m->iSmCaptionWidth = m->iSmCaptionHeight = 15;
        m->iMenuWidth = m->iMenuHeight = 19;
        m->iPaddedBorderWidth = 0;
        default_font(&m->lfCaptionFont, -15);
        default_font(&m->lfSmCaptionFont, -11);
        default_font(&m->lfMenuFont, -12);
        default_font(&m->lfStatusFont, -12);
        default_font(&m->lfMessageFont, -12);
        return TRUE;
    }
    case 0x002D: { /* SPI_GETICONMETRICS */
        ICONMETRICS_W *m = pv;
        if (!m || m->cbSize < sizeof *m)
            break;
        m->iHorzSpacing = 75;
        m->iVertSpacing = 75;
        m->iTitleWrap = 1;
        default_font(&m->lfFont, -12);
        return TRUE;
    }
    case 0x001F: /* SPI_GETICONTITLELOGFONT */
        if (!pv)
            break;
        default_font(pv, -12);
        return TRUE;
    }
    /* a getter that could not fill its buffer fails; setters and other actions are accepted and
     * not persisted */
    static const UINT getters[] = {0x0030, 0x0029, 0x002D, 0x001F};
    for (size_t i = 0; i < sizeof getters / sizeof *getters; i++)
        if (action == getters[i]) {
            aw_set_last_error(87);
            return FALSE;
        }
    return TRUE;
}
