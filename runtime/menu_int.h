/* anywin - menu internals shared by menu.c (object model, item API) and menu_popup.c
 * (TrackPopupMenu, popup class "#32768"). Work package G subset (S6). */
#ifndef AW_MENU_INT_H
#define AW_MENU_INT_H
#include "awmd.h"
#include <stdio.h>

/* user32 / gdi32 entry points and state used by the menu and message box code */
UD(int, FillRect, (HDC, const RECT *, HBRUSH));
extern int aw_display;      /* user32_int.h: plat_init() succeeded */
void aw_user_init(void);    /* user32_wnd.c */
extern HDC WINAPI __aw_gdi32_CreateCompatibleDC(HDC);
extern BOOL WINAPI __aw_gdi32_DeleteDC(HDC);

#define AW_MENU_MAGIC 0x554e454d
#define AW_MENU_CLASS u"#32768"

typedef struct aw_mitem {
    UINT id;          /* command id (0 for popups, bitmaps and owner-draw items) */
    UINT fType;       /* MFT_* bits: MFT_STRING(0), MFT_BITMAP, MFT_SEPARATOR, MFT_OWNERDRAW, ... */
    UINT fState;      /* MFS_* bits: MFS_CHECKED, MFS_GRAYED, MFS_HILITE, MFS_DEFAULT */
    int popup;        /* MF_POPUP: sub is a submenu handle */
    WCHAR *text;      /* raw caption incl. '&' mnemonics and "\taccel" text, NULL when none */
    HMENU sub;
    ULONG_PTR data;
    HBITMAP bmpChecked, bmpUnchecked, bmpItem;
} aw_mitem;

typedef struct aw_menu {
    uint32_t magic;
    int popup;
    int issystem;
    aw_mitem *it;
    int n, cap;
    UINT defid;
    AW_MENUINFO info;
    HMENU self;
} aw_menu;

/* menu.c */
aw_menu *aw_menu_get(HMENU h);             /* NULL when h is not a live menu */

/* menu_popup.c */
void aw_menus_register(void);              /* class "#32768" (strong definition overrides user32_nc.c's weak stub) */

#endif
