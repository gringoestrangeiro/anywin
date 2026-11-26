/* anywin - menu object model and item API (work package G subset, used by S6).
 * HMENUs are 1-based indices into a global table (they fit in 32 bits, as the contract requires).
 * Popup tracking (TrackPopupMenu) lives in menu_popup.c; the menu bar hooks are still deferred
 * (user32_nc.c provides weak no-op defaults). */
#include "menu_int.h"

static aw_menu **g_menus;
static int g_menus_n, g_menus_cap;

#define ITEM_TYPE_MASK (MFT_BITMAP | MFT_MENUBARBREAK | MFT_MENUBREAK | MFT_OWNERDRAW | MFT_RADIOCHECK | \
                        MFT_SEPARATOR | MFT_RIGHTJUSTIFY)
#define ITEM_STATE_MASK (MFS_CHECKED | MFS_GRAYED | MFS_HILITE | MFS_DEFAULT)

aw_menu *aw_menu_get(HMENU h)
{
    uintptr_t i = (uintptr_t)h;
    if (!i || i > (uintptr_t)g_menus_n)
        return NULL;
    aw_menu *m = g_menus[i - 1];
    return (m && m->magic == AW_MENU_MAGIC) ? m : NULL;
}

static HMENU menu_register_slot(aw_menu *m)
{
    int slot = -1;
    for (int i = 0; i < g_menus_n; i++)
        if (!g_menus[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        if (g_menus_n == g_menus_cap) {
            int nc = g_menus_cap ? g_menus_cap * 2 : 64;
            aw_menu **nm = realloc(g_menus, (size_t)nc * sizeof *nm);
            if (!nm)
                return NULL;
            memset(nm + g_menus_cap, 0, (size_t)(nc - g_menus_cap) * sizeof *nm);
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
    m->magic = AW_MENU_MAGIC;
    m->popup = popup;
    m->info.cbSize = sizeof(AW_MENUINFO);
    if (!menu_register_slot(m)) {
        free(m);
        return NULL;
    }
    return m;
}

static void menu_free(aw_menu *m)
{
    for (int i = 0; i < m->n; i++) {
        aw_mitem *it = &m->it[i];
        if (it->popup && it->sub) {
            aw_menu *sm = aw_menu_get(it->sub);
            if (sm)
                menu_free(sm);
        }
        free(it->text);
    }
    free(m->it);
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

/* Inserts at pos (-1 or n = append), taking ownership of src->text on success only. */
static BOOL menu_insert(aw_menu *m, int pos, const aw_mitem *src)
{
    if (pos < 0 || pos > m->n)
        pos = m->n;
    if (!menu_reserve(m, m->n + 1))
        return FALSE;
    memmove(&m->it[pos + 1], &m->it[pos], (size_t)(m->n - pos) * sizeof *m->it);
    m->n++;
    m->it[pos] = *src;
    return TRUE;
}

/* Builds an item from MF_* flags. text is the caption for strings and popups, a bitmap handle for
 * MF_BITMAP and the item data for MF_OWNERDRAW (Windows overloads lpNewItem this way). */
static int item_from_flags(aw_mitem *it, UINT flags, UINT_PTR id, const void *text, int text_is_wide)
{
    memset(it, 0, sizeof *it);
    it->fType = flags & ITEM_TYPE_MASK;
    if (flags & (MF_GRAYED | MF_DISABLED)) it->fState |= MFS_GRAYED;
    if (flags & MF_CHECKED) it->fState |= MFS_CHECKED;
    if (flags & MF_HILITE) it->fState |= MFS_HILITE;
    if (flags & MF_DEFAULT) it->fState |= MFS_DEFAULT;
    if (flags & MF_POPUP) {
        it->popup = 1;
        it->sub = (HMENU)(uintptr_t)id;
        it->fType = flags & (MFT_MENUBARBREAK | MFT_MENUBREAK | MFT_RIGHTJUSTIFY);
        if (text)
            item_set_text(it, text_is_wide ? (const WCHAR *)text : NULL, -1);
        return 1;
    }
    if (flags & MF_SEPARATOR) {
        it->fType = MFT_SEPARATOR;
        return 1;
    }
    it->id = (UINT)id;
    if (flags & MF_BITMAP) {
        it->bmpItem = (HBITMAP)text;
        it->id = (UINT)id;
        return 1;
    }
    if (flags & MF_OWNERDRAW) {
        it->data = (ULONG_PTR)text;
        return 1;
    }
    if (text_is_wide)
        item_set_text(it, text ? (const WCHAR *)text : NULL, -1);
    return 1;
}

/* Position of an item: MF_BYPOSITION index or MF_BYCOMMAND id (popups never match by id). -1 if none. */
static int find_item(aw_menu *m, UINT item, UINT flags)
{
    if (flags & MF_BYPOSITION)
        return item < (UINT)m->n ? (int)item : -1;
    for (int i = 0; i < m->n; i++)
        if (!m->it[i].popup && m->it[i].id == item)
            return i;
    return -1;
}

static HMENU create_menu(int popup)
{
    aw_menu *m = menu_new(popup);
    return m ? m->self : NULL;
}

API(user32, HMENU, CreateMenu, (void)) { return create_menu(0); }
API(user32, HMENU, CreatePopupMenu, (void)) { return create_menu(1); }

API(user32, BOOL, DestroyMenu, (HMENU hMenu))
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m)
        return FALSE;
    menu_free(m);
    return TRUE;
}

API(user32, BOOL, IsMenu, (HMENU hMenu)) { return aw_menu_get(hMenu) != NULL; }

/* ---- insertion ------------------------------------------------------- */
static BOOL insert_flags(HMENU hMenu, int pos, UINT flags, UINT_PTR id, const void *text, int wide)
{
    aw_menu *m = aw_menu_get(hMenu);
    aw_mitem it;
    if (!m)
        return FALSE;
    item_from_flags(&it, flags, id, text, wide);
    if (it.popup && !aw_menu_get(it.sub)) {
        free(it.text);
        return FALSE;
    }
    if (!menu_insert(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

/* text must be converted to UTF-16 by the caller when ANSI */
static BOOL insert_at(HMENU hMenu, UINT position, UINT flags, UINT_PTR id, LPCSTR ansi, LPCWSTR wide, int is_wide)
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos;
    if (flags & MF_BYPOSITION) {
        if (position == (UINT)-1) {
            pos = -1;
        } else if (position > (UINT)m->n) {
            return FALSE;
        } else {
            pos = (int)position;
        }
    } else {
        pos = find_item(m, position, 0);
        if (pos < 0)
            pos = -1; /* not found: append */
    }
    if (is_wide) {
        return insert_flags(hMenu, pos, flags, id, wide, 1);
    }
    WCHAR *w = NULL;
    const void *text = NULL;
    int wide_text = 0;
    if (ansi && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
        w = aw_u2w(ansi, -1);
        text = w;
        wide_text = 1;
    } else {
        text = ansi;
    }
    BOOL ok = insert_flags(hMenu, pos, flags, id, text, wide_text);
    free(w);
    return ok;
}

API(user32, BOOL, AppendMenuA, (HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    return insert_at(hMenu, (UINT)-1, uFlags | MF_BYPOSITION, uIDNewItem, lpNewItem, NULL, 0);
}

API(user32, BOOL, AppendMenuW, (HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    return insert_at(hMenu, (UINT)-1, uFlags | MF_BYPOSITION, uIDNewItem, NULL, lpNewItem, 1);
}

API(user32, BOOL, InsertMenuA, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    return insert_at(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem, NULL, 0);
}

API(user32, BOOL, InsertMenuW, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    return insert_at(hMenu, uPosition, uFlags, uIDNewItem, NULL, lpNewItem, 1);
}

/* MENUITEMINFO insertion; text is the already-converted caption (or NULL) */
static BOOL insert_mii(HMENU hMenu, UINT item, BOOL bypos, const AW_MENUITEMINFOW *mi, const WCHAR *text, int tlen)
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m || !mi)
        return FALSE;
    int pos = bypos ? (int)item : find_item(m, item, 0);
    if (bypos && item > (UINT)m->n)
        return FALSE;
    if (pos < 0)
        pos = -1;
    aw_mitem it;
    memset(&it, 0, sizeof it);
    UINT mask = mi->fMask;
    it.fType = MFT_STRING;
    if (mask & (MIIM_FTYPE | MIIM_TYPE))
        it.fType = mi->fType & ITEM_TYPE_MASK;
    if (mask & MIIM_ID)
        it.id = mi->wID;
    if (mask & MIIM_STATE)
        it.fState = mi->fState & ITEM_STATE_MASK;
    if (mask & MIIM_SUBMENU) {
        it.sub = mi->hSubMenu;
        it.popup = mi->hSubMenu != NULL;
    }
    if (mask & MIIM_CHECKMARKS) {
        it.bmpChecked = mi->hbmpChecked;
        it.bmpUnchecked = mi->hbmpUnchecked;
    }
    if (mask & MIIM_DATA)
        it.data = mi->dwItemData;
    if (mask & MIIM_BITMAP)
        it.bmpItem = mi->hbmpItem;
    if (text && (mask & (MIIM_STRING | MIIM_TYPE)) && !(it.fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)))
        item_set_text(&it, text, tlen);
    if (it.popup && !aw_menu_get(it.sub)) {
        free(it.text);
        return FALSE;
    }
    if (!menu_insert(m, pos, &it)) {
        free(it.text);
        return FALSE;
    }
    return TRUE;
}

API(user32, BOOL, InsertMenuItemW, (HMENU hMenu, UINT uItem, BOOL fByPosition, const AW_MENUITEMINFOW *lpmii))
{
    if (!lpmii)
        return FALSE;
    int tlen = lpmii->cch ? (int)lpmii->cch : -1;
    return insert_mii(hMenu, uItem, fByPosition, lpmii, lpmii->dwTypeDataW, tlen);
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
    w.cch = lpmii->cch;
    w.hbmpItem = lpmii->hbmpItem;
    WCHAR *t = NULL;
    if (lpmii->dwTypeDataA)
        t = aw_u2w(lpmii->dwTypeDataA, lpmii->cch ? (int)lpmii->cch : -1);
    BOOL ok = insert_mii(hMenu, uItem, fByPosition, &w, t, -1);
    free(t);
    return ok;
}

/* ---- modification / removal ------------------------------------------ */
static BOOL modify_at(HMENU hMenu, UINT position, UINT flags, UINT_PTR id, const void *text, int wide)
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = (flags & MF_BYPOSITION) ? (position < (UINT)m->n ? (int)position : -1) : find_item(m, position, 0);
    if (pos < 0)
        return FALSE;
    aw_mitem it;
    if (!item_from_flags(&it, flags, id, text, wide))
        return FALSE;
    if (it.popup && !aw_menu_get(it.sub)) {
        free(it.text);
        return FALSE;
    }
    /* the old item (its caption and, for DeleteMenu-like replacement, not its submenu) is released */
    free(m->it[pos].text);
    m->it[pos] = it;
    return TRUE;
}

API(user32, BOOL, ModifyMenuA, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem))
{
    if (uFlags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR) || !lpNewItem)
        return modify_at(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem, 0);
    WCHAR *w = aw_u2w(lpNewItem, -1);
    BOOL ok = modify_at(hMenu, uPosition, uFlags, uIDNewItem, w, 1);
    free(w);
    return ok;
}

API(user32, BOOL, ModifyMenuW, (HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem))
{
    return modify_at(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem, 1);
}

static BOOL remove_at(HMENU hMenu, UINT position, UINT flags, int destroy_sub)
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m)
        return FALSE;
    int pos = find_item(m, position, flags);
    if (pos < 0)
        return FALSE;
    aw_mitem *it = &m->it[pos];
    if (destroy_sub && it->popup && it->sub) {
        aw_menu *sm = aw_menu_get(it->sub);
        if (sm)
            menu_free(sm);
    }
    free(it->text);
    memmove(&m->it[pos], &m->it[pos + 1], (size_t)(m->n - pos - 1) * sizeof *m->it);
    m->n--;
    return TRUE;
}

API(user32, BOOL, RemoveMenu, (HMENU hMenu, UINT uPosition, UINT uFlags)) { return remove_at(hMenu, uPosition, uFlags, 0); }
API(user32, BOOL, DeleteMenu, (HMENU hMenu, UINT uPosition, UINT uFlags)) { return remove_at(hMenu, uPosition, uFlags, 1); }

/* ---- queries and state ----------------------------------------------- */
API(user32, int, GetMenuItemCount, (HMENU hMenu))
{
    aw_menu *m = aw_menu_get(hMenu);
    return m ? m->n : -1;
}

API(user32, UINT, GetMenuItemID, (HMENU hMenu, int nPos))
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m || nPos < 0 || nPos >= m->n || m->it[nPos].popup)
        return (UINT)-1;
    return m->it[nPos].id;
}

API(user32, HMENU, GetSubMenu, (HMENU hMenu, int nPos))
{
    aw_menu *m = aw_menu_get(hMenu);
    if (!m || nPos < 0 || nPos >= m->n || !m->it[nPos].popup)
        return NULL;
    return aw_menu_get(m->it[nPos].sub) ? m->it[nPos].sub : NULL;
}

API(user32, UINT, GetMenuState, (HMENU hMenu, UINT uId, UINT uFlags))
{
    aw_menu *m = aw_menu_get(hMenu);
    int pos = m ? find_item(m, uId, uFlags) : -1;
    if (pos < 0)
        return (UINT)-1;
    aw_mitem *it = &m->it[pos];
    UINT r = 0;
    if (it->fState & MFS_GRAYED) r |= MF_GRAYED | MF_DISABLED;
    if (it->fState & MFS_CHECKED) r |= MF_CHECKED;
    if (it->fState & MFS_HILITE) r |= MF_HILITE;
    if (it->fState & MFS_DEFAULT) r |= MF_DEFAULT;
    if (it->popup) r |= MF_POPUP;
    if (it->fType & MFT_SEPARATOR) r |= MF_SEPARATOR;
    if (it->fType & MFT_MENUBARBREAK) r |= MF_MENUBARBREAK;
    if (it->fType & MFT_MENUBREAK) r |= MF_MENUBREAK;
    return r;
}

/* sets the enable/check bits of an item; returns the previous MF_* state or -1 */
static UINT set_state_bits(HMENU hMenu, UINT uId, UINT uFlags, UINT bits, int enable_mode)
{
    aw_menu *m = aw_menu_get(hMenu);
    int pos = m ? find_item(m, uId, uFlags) : -1;
    if (pos < 0)
        return (UINT)-1;
    aw_mitem *it = &m->it[pos];
    UINT prev = (it->fState & MFS_GRAYED) ? MF_GRAYED : MF_ENABLED;
    if (bits == MFS_CHECKED)
        prev = (it->fState & MFS_CHECKED) ? MF_CHECKED : MF_UNCHECKED;
    if (enable_mode == 1)
        it->fState |= bits;
    else if (enable_mode == 0)
        it->fState &= ~bits;
    return prev;
}

API(user32, DWORD, CheckMenuItem, (HMENU hMenu, UINT uIDCheckItem, UINT uCheck))
{
    return set_state_bits(hMenu, uIDCheckItem, uCheck & MF_BYPOSITION, MFS_CHECKED, (uCheck & MF_CHECKED) ? 1 : 0);
}

API(user32, BOOL, EnableMenuItem, (HMENU hMenu, UINT uIDEnableItem, UINT uEnable))
{
    int on = (uEnable & (MF_GRAYED | MF_DISABLED)) ? 1 : 0;
    return (BOOL)set_state_bits(hMenu, uIDEnableItem, uEnable & MF_BYPOSITION, MFS_GRAYED, on);
}
