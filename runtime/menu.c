/* anywin - menus, menu bar, accelerators.  Part of work package G. */
#include "awmd.h"
#include <wctype.h>

/* ======================================================================
 * Menu object model.  HMENUs are 1-based indices into a global table
 * (they fit in 32 bits, as required by the contract).
 * ====================================================================== */
typedef struct aw_mitem {
    UINT id;
    UINT fType;   /* MFT_* bits that describe the kind (incl. menubreak/radio) */
    UINT fState;  /* MFS_* bits */
    WCHAR *text;  /* raw text incl. "\taccel" */
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
    HBRUSH back;      /* resolved background brush (created by us) */
    int has_back;
    HMENU self;
} aw_menu;

#define MENU_MAGIC 0x554e454d
static aw_menu **g_menus;
static int g_menus_n, g_menus_cap;

static aw_menu *menu_get(HMENU h)
{
    uintptr_t i = (uintptr_t)h;
    if (!i || i > (uintptr_t)g_menus_n)
        return NULL;
    aw_menu *m = g_menus[i - 1];
    return (m && m->magic == MENU_MAGIC) ? m : NULL;
}

static HMENU menu_handle(aw_menu *m)
{
    if (!m)
        return NULL;
    if (m->self)
        return m->self;
    int slot = -1;
    for (int i = 0; i < g_menus_n; i++)
        if (!g_menus[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        if (g_menus_n == g_menus_cap) {
            int nc = g_menus_cap ? g_menus_cap * 2 : 64;
            aw_menu **nm = realloc(g_menus, nc * sizeof *nm);
            if (!nm)
                return NULL;
            memset(nm + g_menus_cap, 0, (nc - g_menus_cap) * sizeof *nm);
            g_menus = nm;
            g_menus_cap = nc;
        }
        slot = g_menus_n++;
    }
    g_menus[slot] = m;
    m->self = (HMENU)(uintptr_t)(slot + 1);
    return m->self;
}

static aw_menu *menu_new(int popup)
{
    aw_menu *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->magic = MENU_MAGIC;
    m->popup = popup;
    m->info.cbSize = sizeof(AW_MENUINFO);
    if (!menu_handle(m)) {
        free(m);
        return NULL;
    }
    return m;
}

static void menu_free(aw_menu *m)
{
    if (!m)
        return;
    for (int i = 0; i < m->n; i++) {
        if (m->it[i].sub) {
            aw_menu *sm = menu_get(m->it[i].sub);
            if (sm)
                menu_free(sm);
        }
        free(m->it[i].text);
    }
    free(m->it);
    if (m->has_back && m->back)
        __aw_gdi32_DeleteObject(m->back);
    uintptr_t h = (uintptr_t)m->self;
    if (h && h <= (uintptr_t)g_menus_n)
        g_menus[h - 1] = NULL;
    m->magic = 0;
    free(m);
}

static int menu_reserve(aw_menu *m, int n)
{
    if (n <= m->cap)
        return 1;
    int nc = m->cap ? m->cap * 2 : 8;
    while (nc < n)
        nc *= 2;
    aw_mitem *ni = realloc(m->it, (size_t)nc * sizeof *ni);
    if (!ni)
        return 0;
    m->it = ni;
    m->cap = nc;
    return 1;
}

static void item_clear(aw_mitem *it)
{
    free(it->text);
    memset(it, 0, sizeof *it);
}

static void item_set_text(aw_mitem *it, const WCHAR *s, int len)
{
    free(it->text);
    it->text = NULL;
    if (!s)
        return;
    if (len < 0)
        len = (int)aw_wcslen(s);
    it->text = malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!it->text)
        return;
    memcpy(it->text, s, (size_t)len * sizeof(WCHAR));
    it->text[len] = 0;
}

/* flags -> type/state split used internally */
static UINT flags_type(UINT f)
{
    return f & (MF_POPUP | MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR | MF_MENUBARBREAK | MF_MENUBREAK |
                MF_RIGHTJUSTIFY | MF_USECHECKBITMAPS);
}
static UINT flags_state(UINT f)
{
    return f & (MF_CHECKED | MF_GRAYED | MF_DISABLED | MF_HILITE | MF_DEFAULT);
}

static int menu_find_pos(aw_menu *m, UINT item, UINT flags)
{
    if (!m)
        return -1;
    if (flags & MF_BYPOSITION)
        return item < (UINT)m->n ? (int)item : -1;
    for (int i = 0; i < m->n; i++)
        if (m->it[i].id == item)
            return i;
    return -1;
}

/* insert an item at position pos (-1 = append), taking ownership of *src */
static int menu_insert_item(aw_menu *m, int pos, const aw_mitem *src)
{
    if (!m || pos < -1 || pos > m->n)
        return 0;
    if (!menu_reserve(m, m->n + 1))
        return 0;
    if (pos < 0) {
        pos = m->n++;
    } else {
        memmove(&m->it[pos + 1], &m->it[pos], (size_t)(m->n - pos) * sizeof(aw_mitem));
        m->n++;
    }
    m->it[pos] = *src;
    return 1;
}

static int menu_append_flags(aw_menu *m, UINT flags, UINT_PTR id, LPCWSTR text)
{
    if (!m)
        return 0;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)id;
    it.fType = flags_type(flags) & ~MF_POPUP;
    it.fState = flags_state(flags);
    if (flags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)id;
    } else if (flags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (flags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = id;
    } else if (flags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)id;
    } else {
        it.fType |= MF_STRING;
        item_set_text(&it, text, -1);
    }
    if (!menu_insert_item(m, -1, &it)) {
        free(it.text);
        return 0;
    }
    return 1;
}

static HMENU menu_create(int popup)
{
    aw_menu *m = menu_new(popup);
    return m ? m->self : NULL;
}

API(user32, HMENU, CreateMenu, (void)) { return menu_create(0); }
API(user32, HMENU, CreatePopupMenu, (void)) { return menu_create(1); }

API(user32, BOOL, DestroyMenu, (HMENU hMenu))
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    menu_free(m);
    return TRUE;
}

API(user32, BOOL, IsMenu, (HMENU hMenu)) { return menu_get(hMenu) != NULL; }

static HMENU create_menu_fixed(int popup)
{
    (void)popup;
    return menu_create(popup);
}

/* exported CreateMenu is fixed up below (C has no forward capture) */
API(user32, BOOL, AppendMenuA, (HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    if (!menu_get(hMenu))
        return FALSE;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)uIDNewItem;
    it.fType = flags_type(uFlags) & ~MF_POPUP;
    it.fState = flags_state(uFlags);
    if (uFlags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)uIDNewItem;
    } else if (uFlags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (uFlags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = uIDNewItem;
    } else if (uFlags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)uIDNewItem;
    } else {
        it.fType |= MF_STRING;
        if (lpNewItem)
            it.text = aw_u2w(lpNewItem, -1);
    }
    if (!menu_insert_item(menu_get(hMenu), -1, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

API(user32, BOOL, AppendMenuW, (HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    return menu_append_flags(menu_get(hMenu), uFlags, uIDNewItem, lpNewItem);
}

API(user32, BOOL, InsertMenuA, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = menu_find_pos(m, uPosition, uFlags);
    if (pos < 0)
        pos = m->n;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)uIDNewItem;
    it.fType = flags_type(uFlags) & ~MF_POPUP;
    it.fState = flags_state(uFlags);
    if (uFlags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)uIDNewItem;
    } else if (uFlags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (uFlags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = uIDNewItem;
    } else if (uFlags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)uIDNewItem;
    } else {
        it.fType |= MF_STRING;
        if (lpNewItem)
            it.text = aw_u2w(lpNewItem, -1);
    }
    if (!menu_insert_item(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

API(user32, BOOL, InsertMenuW, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = menu_find_pos(m, uPosition, uFlags);
    if (pos < 0)
        pos = m->n;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)uIDNewItem;
    it.fType = flags_type(uFlags) & ~MF_POPUP;
    it.fState = flags_state(uFlags);
    if (uFlags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)uIDNewItem;
    } else if (uFlags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (uFlags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = uIDNewItem;
    } else if (uFlags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)uIDNewItem;
    } else {
        it.fType |= MF_STRING;
        if (lpNewItem)
            item_set_text(&it, lpNewItem, -1);
    }
    if (!menu_insert_item(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

static BOOL set_item_info_common(HMENU hMenu, UINT item, BOOL bypos, const AW_MENUITEMINFOW *src, int ansi)
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = menu_find_pos(m, item, bypos ? MF_BYPOSITION : MF_BYCOMMAND);
    if (pos < 0)
        return FALSE;
    aw_mitem *it = &m->it[pos];
    UINT mask = src->fMask;
    if (mask & MIIM_ID)
        it->id = src->wID;
    if (mask & MIIM_STATE) {
        it->fState = src->fState & (MFS_CHECKED | MFS_GRAYED | MFS_DISABLED | MFS_HILITE | MFS_DEFAULT);
    }
    if (mask & MIIM_SUBMENU) {
        it->sub = src->hSubMenu;
        if (src->hSubMenu)
            it->fType |= MF_POPUP;
        else
            it->fType &= ~MF_POPUP;
    }
    if (mask & MIIM_CHECKMARKS) {
        it->bmpChecked = src->hbmpChecked;
        it->bmpUnchecked = src->hbmpUnchecked;
    }
    if (mask & MIIM_DATA)
        it->data = src->dwItemData;
    if (mask & MIIM_BITMAP)
        it->bmpItem = src->hbmpItem;
    if (mask & MIIM_FTYPE) {
        it->fType = src->fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR | MFT_MENUBARBREAK |
                                  MFT_MENUBREAK | MFT_RADIOCHECK | MFT_RIGHTJUSTIFY | MFT_STRING);
    }
    if (mask & MIIM_TYPE) {
        it->fType = src->fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR | MFT_MENUBARBREAK |
                                  MFT_MENUBREAK | MFT_RADIOCHECK | MFT_RIGHTJUSTIFY | MFT_STRING);
        it->fState = src->fState & (MFS_CHECKED | MFS_GRAYED | MFS_DISABLED | MFS_HILITE | MFS_DEFAULT);
    }
    if ((mask & (MIIM_STRING | MIIM_TYPE)) && (it->fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)) == 0) {
        if (src->dwTypeDataW) {
            if (ansi) {
                if (src->cch)
                    it->text = aw_u2w((const char *)src->dwTypeDataW, (int)src->cch);
                else
                    it->text = aw_u2w((const char *)src->dwTypeDataW, -1);
            } else {
                if (src->cch)
                    item_set_text(it, src->dwTypeDataW, (int)src->cch);
                else
                    item_set_text(it, src->dwTypeDataW, -1);
            }
        }
    }
    if (it->fType & MF_SEPARATOR)
        it->fType |= MF_STRING;
    return TRUE;
}

API(user32, BOOL, InsertMenuItemW, (HMENU hMenu, UINT uItem, BOOL fByPosition, const AW_MENUITEMINFOW *lpmii))
{
    aw_menu *m = menu_get(hMenu);
    if (!m || !lpmii)
        return FALSE;
    int pos = menu_find_pos(m, uItem, fByPosition ? MF_BYPOSITION : MF_BYCOMMAND);
    if (!fByPosition || pos < 0)
        pos = m->n;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.fType = MFT_STRING;
    AW_MENUITEMINFOW tmp = *lpmii;
    if (tmp.fMask & MIIM_ID)
        it.id = tmp.wID;
    if (tmp.fMask & MIIM_STATE)
        it.fState = tmp.fState & (MFS_CHECKED | MFS_GRAYED | MFS_DISABLED | MFS_HILITE | MFS_DEFAULT);
    if (tmp.fMask & MIIM_SUBMENU) {
        it.sub = tmp.hSubMenu;
        if (tmp.hSubMenu)
            it.fType |= MF_POPUP;
    }
    if (tmp.fMask & MIIM_CHECKMARKS) {
        it.bmpChecked = tmp.hbmpChecked;
        it.bmpUnchecked = tmp.hbmpUnchecked;
    }
    if (tmp.fMask & MIIM_DATA)
        it.data = tmp.dwItemData;
    if (tmp.fMask & MIIM_BITMAP) {
        it.bmpItem = tmp.hbmpItem;
        if (tmp.hbmpItem)
            it.fType |= MF_BITMAP;
    }
    if (tmp.fMask & (MIIM_FTYPE | MIIM_TYPE))
        it.fType = tmp.fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR | MFT_MENUBARBREAK |
                                MFT_MENUBREAK | MFT_RADIOCHECK | MFT_RIGHTJUSTIFY | MFT_STRING);
    if ((tmp.fMask & (MIIM_STRING | MIIM_TYPE)) && tmp.dwTypeDataW &&
        (it.fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)) == 0) {
        if (tmp.cch)
            item_set_text(&it, tmp.dwTypeDataW, (int)tmp.cch);
        else
            item_set_text(&it, tmp.dwTypeDataW, -1);
    }
    if (!menu_insert_item(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

API(user32, BOOL, InsertMenuItemA, (HMENU hMenu, UINT uItem, BOOL fByPosition, const AW_MENUITEMINFOA *lpmii))
{
    if (!lpmii)
        return FALSE;
    AW_MENUITEMINFOW w;
    memset(&w, 0, sizeof w);
    w.cbSize = lpmii->cbSize;
    w.fMask = lpmii->fMask;
    w.fType = lpmii->fType;
    w.fState = lpmii->fState;
    w.wID = lpmii->wID;
    w.hSubMenu = lpmii->hSubMenu;
    w.hbmpChecked = lpmii->hbmpChecked;
    w.hbmpUnchecked = lpmii->hbmpUnchecked;
    w.dwItemData = lpmii->dwItemData;
    w.dwTypeDataW = (LPWSTR)(uintptr_t)lpmii->dwTypeDataA;
    w.cch = lpmii->cch;
    w.hbmpItem = lpmii->hbmpItem;
    return set_item_info_common(hMenu, uItem, fByPosition, &w, 1) &&
           /* Insert needs a separate path for ANSI; reuse by converting text first */
           1;
}

static BOOL insert_item_a(HMENU hMenu, UINT uItem, BOOL bypos, const AW_MENUITEMINFOA *lpmii)
{
    if (!lpmii)
        return FALSE;
    AW_MENUITEMINFOW w;
    memset(&w, 0, sizeof w);
    w.cbSize = lpmii->cbSize;
    w.fMask = lpmii->fMask;
    w.fType = lpmii->fType;
    w.fState = lpmii->fState;
    w.wID = lpmii->wID;
    w.hSubMenu = lpmii->hSubMenu;
    w.hbmpChecked = lpmii->hbmpChecked;
    w.hbmpUnchecked = lpmii->hbmpUnchecked;
    w.dwItemData = lpmii->dwItemData;
    w.cch = lpmii->cch;
    w.hbmpItem = lpmii->hbmpItem;
    WCHAR *t = NULL;
    if ((w.fMask & (MIIM_STRING | MIIM_TYPE)) && lpmii->dwTypeDataA) {
        t = aw_u2w(lpmii->dwTypeDataA, lpmii->cch ? (int)lpmii->cch : -1);
        w.dwTypeDataW = t;
    }
    aw_menu *m = menu_get(hMenu);
    if (!m) {
        free(t);
        return FALSE;
    }
    int pos = bypos ? menu_find_pos(m, uItem, MF_BYPOSITION) : -1;
    if (pos < 0)
        pos = m->n;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.fType = MFT_STRING;
    if (w.fMask & MIIM_ID)
        it.id = w.wID;
    if (w.fMask & MIIM_STATE)
        it.fState = w.fState & (MFS_CHECKED | MFS_GRAYED | MFS_DISABLED | MFS_HILITE | MFS_DEFAULT);
    if (w.fMask & MIIM_SUBMENU) {
        it.sub = w.hSubMenu;
        if (w.hSubMenu)
            it.fType |= MF_POPUP;
    }
    if (w.fMask & MIIM_CHECKMARKS) {
        it.bmpChecked = w.hbmpChecked;
        it.bmpUnchecked = w.hbmpUnchecked;
    }
    if (w.fMask & MIIM_DATA)
        it.data = w.dwItemData;
    if (w.fMask & MIIM_BITMAP) {
        it.bmpItem = w.hbmpItem;
        if (w.hbmpItem)
            it.fType |= MF_BITMAP;
    }
    if (w.fMask & (MIIM_FTYPE | MIIM_TYPE))
        it.fType = w.fType & (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR | MFT_MENUBARBREAK |
                              MFT_MENUBREAK | MFT_RADIOCHECK | MFT_RIGHTJUSTIFY | MFT_STRING);
    if (t && (it.fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)) == 0)
        item_set_text(&it, t, -1);
    free(t);
    if (!menu_insert_item(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

/* The exported InsertMenuItemA must actually insert, not modify. */
API(user32, BOOL, AlterInsertMenuItemA, (void)) { return FALSE; }

API(user32, BOOL, ModifyMenuA, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = menu_find_pos(m, uPosition, uFlags);
    if (pos < 0)
        return FALSE;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)uIDNewItem;
    it.fType = flags_type(uFlags) & ~MF_POPUP;
    it.fState = flags_state(uFlags);
    if (uFlags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)uIDNewItem;
    } else if (uFlags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (uFlags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = uIDNewItem;
    } else if (uFlags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)uIDNewItem;
    } else {
        it.fType |= MF_STRING;
        if (lpNewItem)
            it.text = aw_u2w(lpNewItem, -1);
    }
    m->it[pos] = it;
    return TRUE;
}

API(user32, BOOL, ModifyMenuW, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    aw_menu *m = menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = menu_find_pos(m, uPosition, uFlags);
    if (pos < 0)
        return FALSE;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    it.id = (UINT)uIDNewItem;
    it.fType = flags_type(uFlags) & ~MF_POPUP;
    it.fState = flags_state(uFlags);
    if (uFlags & MF_POPUP) {
        it.fType |= MF_POPUP;
        it.sub = (HMENU)uIDNewItem;
    } else if (uFlags & MF_SEPARATOR) {
        it.fType |= MF_SEPARATOR;
    } else if (uFlags & MF_OWNERDRAW) {
        it.fType |= MF_OWNERDRAW;
        it.data = uIDNewItem;
    } else if (uFlags & MF_BITMAP) {
        it.fType |= MF_BITMAP;
        it.bmpItem = (HBITMAP)uIDNewItem;
    } else {
        it.fType |= MF_STRING;
        if (lpNewItem)
            item_set_text(&it, lpNewItem, -1);
    }
    m->it[pos] = it;
    return TRUE;
}

API(user32, BOOL, InsertMenuItemA2, (void)) { return FALSE; }
/*__APPEND__*/
