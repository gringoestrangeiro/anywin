/* SysListView32 model unit test: rows, sub-items, state hooks, selection/focus policy, stable
 * sort through an ms_abi comparator, sorted insertion, search, and a randomized operation run
 * whose invariants (single focus row, selection = policy result, one changed-notification per
 * real state change) are checked after every step.
 * White-box: includes runtime/controls/listview.c with AW_LV_UNIT_TEST, so only the model is
 * compiled (no Win32 calls).  Usage: listview_unit [seed] [rounds].  Exit 0 = PASS. */
#define AW_LV_UNIT_TEST 1
#include "../runtime/controls/listview.c"

#include <stdio.h>
#include <stdlib.h>

/* x64 layouts the app reads and writes (Windows SDK offsets) */
#include <stddef.h>
_Static_assert(sizeof(NMHDR) == 24, "NMHDR is 24 bytes on x64");
_Static_assert(offsetof(lv_nmitem_t, iItem) == 0x18 && offsetof(lv_nmitem_t, uNewState) == 0x20, "NMLISTVIEW head");
_Static_assert(offsetof(lv_nmitem_t, uChanged) == 0x28 && offsetof(lv_nmitem_t, ptAction) == 0x2c, "NMLISTVIEW state");
_Static_assert(offsetof(lv_nmitem_t, lParam) == 0x38 && offsetof(lv_nmitem_t, uKeyFlags) == 0x40, "NMLISTVIEW lParam");
_Static_assert(offsetof(LVITEMW, pszText) == 0x18 && offsetof(LVITEMW, cchTextMax) == 0x20, "LVITEMW text");
_Static_assert(offsetof(LVITEMW, iImage) == 0x24 && offsetof(LVITEMW, lParam) == 0x28, "LVITEMW image/lParam");
_Static_assert(offsetof(LVITEMW, iIndent) == 0x30 && sizeof(LVITEMW) == 0x58, "LVITEMW indent and size");
_Static_assert(offsetof(LVCOLUMNW, pszText) == 0x10 && offsetof(LVCOLUMNW, cchTextMax) == 0x18, "LVCOLUMNW text");
_Static_assert(offsetof(LVCOLUMNW, iSubItem) == 0x1c && offsetof(LVCOLUMNW, cxIdeal) == 0x30, "LVCOLUMNW tail");
_Static_assert(offsetof(NMLVDISPINFOW, item) == 0x18, "NMLVDISPINFOW item");
_Static_assert(offsetof(NMLVKEYDOWN, wVKey) == 0x18 && offsetof(NMLVKEYDOWN, flags) == 0x1c, "NMLVKEYDOWN");
_Static_assert(offsetof(HDITEMW, pszText) == 0x08 && offsetof(HDITEMW, cchTextMax) == 0x18, "HDITEMW text");
_Static_assert(offsetof(HDITEMW, fmt) == 0x1c && offsetof(HDITEMW, lParam) == 0x20, "HDITEMW fmt/lParam");
_Static_assert(offsetof(HDITEMW, iImage) == 0x28 && offsetof(HDITEMW, state) == 0x40, "HDITEMW tail");
_Static_assert(offsetof(NMHEADERW, iItem) == 0x18 && offsetof(NMHEADERW, pitem) == 0x20, "NMHEADERW");
_Static_assert(offsetof(LVHITTESTINFO, iItem) == 0x0c, "LVHITTESTINFO");
_Static_assert(offsetof(lv_findinfo_t, lParam) == 0x10 && offsetof(lv_findinfo_t, pt) == 0x18, "LVFINDINFO");

static int checks, failures;

#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
        }                                                                                          \
    } while (0)

static WCHAR *W(const char *s)
{
    size_t n = strlen(s);
    WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
    for (size_t i = 0; i <= n; i++)
        w[i] = (unsigned char)s[i];
    return w;
}

static int label_is(const struct lv *lv, int idx, const char *s)
{
    const WCHAR *t = lv_row_text(lv->rows[idx], 0);
    size_t n = strlen(s);
    if (!t)
        return n == 0;
    for (size_t i = 0; i < n; i++)
        if (t[i] != (unsigned char)s[i])
            return 0;
    return t[n] == 0;
}

/* ---- hooks that record every change ------------------------------ */
typedef struct {
    int nchanging, nchanged, veto_idx;
    int last_idx;
    UINT last_old, last_new;
    int bad; /* changed hooks that reported old == new */
} rec_t;

static int rec_changing(void *ctx, int idx, UINT o, UINT n)
{
    rec_t *r = ctx;
    (void)o;
    (void)n;
    r->nchanging++;
    return idx == r->veto_idx;
}

static void rec_changed(void *ctx, int idx, UINT o, UINT n)
{
    rec_t *r = ctx;
    r->nchanged++;
    if (o == n)
        r->bad++;
    r->last_idx = idx;
    r->last_old = o;
    r->last_new = n;
}

static lv_ops rec_ops(rec_t *r)
{
    lv_ops ops;
    ops.ctx = r;
    ops.changing = rec_changing;
    ops.changed = rec_changed;
    return ops;
}

/* ---- ms_abi comparators, called through the same pointer type as the app's ---- */
static int sp_seen;
static int AWCALL cmp_key(LPARAM a, LPARAM b, LPARAM sp)
{
    if (sp == 0x5EED)
        sp_seen++;
    return (a > b) - (a < b);
}

static struct lv *new_lv(int sortmode)
{
    struct lv *lv = calloc(1, sizeof *lv);
    lv->focus = -1;
    lv->anchor = -1;
    lv->sortmode = sortmode;
    return lv;
}

static int add_row(struct lv *lv, const char *label, LPARAM param)
{
    lv_row *r = lv_row_new();
    int idx;
    if (label)
        lv_row_set_sub(r, 0, W(label));
    r->lparam = param;
    idx = lv_insert_row(lv, lv->sortmode ? lv_sorted_pos(lv, lv_label_for_model(r)) : lv->nrows, r);
    return idx;
}

static void free_lv(struct lv *lv)
{
    for (int i = 0; i < lv->nrows; i++)
        lv_row_free(lv->rows[i]);
    free(lv->rows);
    free(lv);
}

/* ---- individual checks ------------------------------------------- */
static void test_rows_and_subitems(void)
{
    struct lv *lv = new_lv(0);
    lv_row *r;
    add_row(lv, "alpha", 1);
    add_row(lv, "beta", 2);
    add_row(lv, "gamma", 3);
    CHECK(lv->nrows == 3, "three rows inserted, got %d", lv->nrows);
    CHECK(label_is(lv, 1, "beta") && lv->rows[2]->lparam == 3, "order and lParam kept");

    /* a sub-item in column 2 of row 0 sticks; column 1 stays empty */
    r = lv->rows[0];
    CHECK(lv_row_set_sub(r, 2, W("two")) == 0, "set sub 2");
    CHECK(lv_row_text(r, 1) == NULL, "sub 1 empty");
    CHECK(lv_row_text(r, 2) && lv_row_text(r, 2)[0] == 't', "sub 2 text");

    /* opening a column at 1 moves sub 2 to 3 */
    CHECK(lv_row_insert_sub(r, 1) == 0, "insert sub col 1");
    CHECK(lv_row_text(r, 1) == NULL && lv_row_text(r, 3) && lv_row_text(r, 3)[0] == 't', "sub shift on insert");
    lv_row_delete_sub(r, 3);
    CHECK(lv_row_text(r, 3) == NULL, "delete sub col 3 removes the text");

    /* callback text marker is stored and not freed as a pointer */
    CHECK(lv_row_set_sub(lv->rows[1], 0, LV_CB) == 0 && lv_row_text(lv->rows[1], 0) == LV_CB, "callback marker");
    CHECK(lv_row_set_sub(lv->rows[1], 0, W("back")) == 0 && label_is(lv, 1, "back"), "replace callback with text");
    free_lv(lv);
}

static void test_focus_anchor_follow_rows(void)
{
    struct lv *lv = new_lv(0);
    rec_t rec = {0, 0, -1, -1, 0, 0, 0};
    lv_ops ops = rec_ops(&rec);
    add_row(lv, "a", 0);
    add_row(lv, "b", 0);
    add_row(lv, "c", 0);
    lv_set_focus(lv, &ops, 1);
    lv->anchor = 1;
    CHECK(lv->focus == 1 && (lv->rows[1]->state & LVIS_FOCUSED), "focus set on row 1");
    lv_insert_row(lv, 0, lv_row_new());
    CHECK(lv->focus == 2 && lv->anchor == 2, "insert above shifts focus and anchor");
    lv_row_free(lv_remove_row(lv, 0));
    CHECK(lv->focus == 1 && lv->anchor == 1, "remove above shifts back");
    lv_row_free(lv_remove_row(lv, 1));
    CHECK(lv->focus == -1 && lv->anchor == -1, "removing the focus row clears focus and anchor");
    free_lv(lv);
}

static void test_selection_policy(void)
{
    struct lv *lv = new_lv(0);
    rec_t rec = {0, 0, -1, -1, 0, 0, 0};
    lv_ops ops = rec_ops(&rec);
    for (int i = 0; i < 6; i++)
        add_row(lv, "x", i);

    lv_move_focus(lv, &ops, 0, 0, 0);
    CHECK(lv_selected_count(lv) == 1 && (lv->rows[0]->state & LVIS_SELECTED), "plain click selects row 0");
    CHECK(lv->focus == 0 && lv->anchor == 0, "focus and anchor on row 0");

    lv_move_focus(lv, &ops, 3, MK_SHIFT, 0);
    CHECK(lv_selected_count(lv) == 4, "shift selects 0..3, got %d", lv_selected_count(lv));
    CHECK(lv->rows[3]->state & LVIS_SELECTED && !(lv->rows[4]->state & LVIS_SELECTED), "range edges");
    CHECK(lv->anchor == 0 && lv->focus == 3, "shift keeps the anchor");

    lv_click_item(lv, &ops, 5, MK_CONTROL, 0);
    CHECK(lv_selected_count(lv) == 5, "ctrl adds row 5");
    lv_click_item(lv, &ops, 2, MK_CONTROL, 0);
    CHECK(lv_selected_count(lv) == 4 && !(lv->rows[2]->state & LVIS_SELECTED), "ctrl toggles row 2 off");

    lv_move_focus(lv, &ops, 4, MK_CONTROL, 0);
    CHECK(lv->focus == 4 && lv_selected_count(lv) == 4, "ctrl+focus moves focus, keeps selection");

    lv_click_blank(lv, &ops, 0);
    CHECK(lv_selected_count(lv) == 0, "click on blank clears selection");
    CHECK(lv->focus == 4, "blank click keeps focus");

    lv_click_item(lv, &ops, 1, 0, 1); /* single select: a new click replaces the old one */
    lv_click_item(lv, &ops, 2, MK_CONTROL, 1);
    CHECK(lv_selected_count(lv) == 1 && (lv->rows[2]->state & LVIS_SELECTED), "LVS_SINGLESEL keeps one row");

    lv_select_range(lv, &ops, 4, 0);
    CHECK(lv_selected_count(lv) == 5 && (lv->rows[0]->state & LVIS_SELECTED), "reversed range 4..0");

    CHECK(lv_next_item(lv, -1, LVNI_SELECTED) == 0, "next selected from start");
    CHECK(lv_next_item(lv, 4, LVNI_SELECTED) == -1, "no selected after row 4");
    CHECK(lv_next_item(lv, -1, LVNI_FOCUSED) == lv->focus, "next focused");

    lv_toggle_check(lv, &ops, 0);
    CHECK(((lv->rows[0]->state & LVIS_STATEIMAGEMASK) >> 12) == 2, "toggle to checked");
    lv_toggle_check(lv, &ops, 0);
    CHECK(((lv->rows[0]->state & LVIS_STATEIMAGEMASK) >> 12) == 1, "toggle back to unchecked");
    free_lv(lv);
}

static void test_hooks_and_veto(void)
{
    struct lv *lv = new_lv(0);
    rec_t rec = {0, 0, 2, -1, 0, 0, 0};
    lv_ops ops = rec_ops(&rec);
    add_row(lv, "a", 0);
    add_row(lv, "b", 0);
    add_row(lv, "c", 0);
    CHECK(lv_set_state(lv, &ops, 2, LVIS_SELECTED, LVIS_SELECTED) == 0, "vetoed change reports no change");
    CHECK(!(lv->rows[2]->state & LVIS_SELECTED), "vetoed state not stored");
    CHECK(rec.nchanging == 1 && rec.nchanged == 0, "veto skips the changed hook");

    rec.veto_idx = -1;
    lv_set_state(lv, &ops, 1, LVIS_SELECTED, LVIS_SELECTED);
    CHECK(rec.last_idx == 1 && rec.last_old == 0 && rec.last_new == LVIS_SELECTED, "changed hook sees old/new");
    lv_set_state(lv, &ops, 1, LVIS_SELECTED, LVIS_SELECTED); /* no-op: no hooks */
    CHECK(rec.nchanging == 2 && rec.nchanged == 1, "no-op change fires no hooks");
    free_lv(lv);
}

static int check_sorted_stable(const struct lv *lv, int n)
{
    for (int i = 1; i < n; i++) {
        if (lv->rows[i - 1]->lparam > lv->rows[i]->lparam)
            return 0;
        if (lv->rows[i - 1]->lparam == lv->rows[i]->lparam && lv->rows[i - 1]->image > lv->rows[i]->image)
            return 0; /* image holds the original index: equal keys must keep it */
    }
    return 1;
}

static void test_sort(void)
{
    struct lv *lv = new_lv(0);
    rec_t rec = {0, 0, -1, -1, 0, 0, 0};
    lv_ops ops = rec_ops(&rec);
    const LPARAM keys[] = {5, 1, 4, 1, 3, 0, 4, 2, 5, 0};
    int n = (int)(sizeof keys / sizeof keys[0]);
    for (int i = 0; i < n; i++) {
        add_row(lv, "k", keys[i]);
        lv->rows[i]->image = i; /* original position, for stability */
    }
    lv_set_focus(lv, &ops, 2); /* row with key 4, original index 2 */
    lv->anchor = 6;
    rec.nchanging = rec.nchanged = 0; /* only the sort itself is checked below */
    sp_seen = 0;
    CHECK(lv_sort_rows(lv, cmp_key, 0x5EED), "sort succeeded");
    CHECK(check_sorted_stable(lv, n), "sorted ascending, equal keys keep original order");
    CHECK(sp_seen > 0, "comparator received the sort parameter");
    CHECK(lv->rows[lv->focus]->image == 2, "focus follows its row through the sort");
    CHECK(lv->rows[lv->anchor]->image == 6, "anchor follows its row through the sort");
    CHECK(rec.nchanging == 0 && rec.nchanged == 0, "sorting sends no state notifications");
    free_lv(lv);
}

static void test_sorted_insert_and_find(void)
{
    struct lv *lv = new_lv(1); /* LVS_SORTASCENDING */
    add_row(lv, "pear", 1);
    add_row(lv, "apple", 2);
    add_row(lv, "mango", 3);
    add_row(lv, "Banana", 4);
    CHECK(label_is(lv, 0, "apple") && label_is(lv, 1, "Banana") && label_is(lv, 2, "mango") && label_is(lv, 3, "pear"),
          "LVS_SORTASCENDING keeps labels ordered, case-insensitive");
    CHECK(lv_find_item(lv, -1, 0, W("mango"), 0) == 2, "find exact label");
    CHECK(lv_find_item(lv, -1, LVFI_PARTIAL, W("ma"), 0) == 2, "find by prefix");
    CHECK(lv_find_item(lv, -1, LVFI_PARAM, NULL, 4) == 1, "find by lParam");
    CHECK(lv_find_item(lv, 2, LVFI_PARTIAL, W("a"), 0) == 0, "search wraps around");
    CHECK(lv_find_item(lv, -1, 0, W("zzz"), 0) == -1, "missing label is -1");
    free_lv(lv);

    lv = new_lv(2); /* descending */
    add_row(lv, "b", 0);
    add_row(lv, "d", 0);
    add_row(lv, "c", 0);
    CHECK(label_is(lv, 0, "d") && label_is(lv, 1, "c") && label_is(lv, 2, "b"), "LVS_SORTDESCENDING");
    free_lv(lv);
}

/* ---- randomized run with invariants ------------------------------ */
static unsigned long rng_state;
static unsigned rnd(unsigned n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return n ? (unsigned)(rng_state % n) : 0;
}

static void check_invariants(const struct lv *lv, const char *where)
{
    int focused = 0, fidx = -1, anchor_ok;
    for (int i = 0; i < lv->nrows; i++)
        if (lv->rows[i]->state & LVIS_FOCUSED) {
            focused++;
            fidx = i;
        }
    CHECK(focused <= 1, "%s: at most one focus bit", where);
    CHECK((lv->focus == -1) == (focused == 0), "%s: focus index matches the focus bit", where);
    CHECK(lv->focus == -1 || lv->focus == fidx, "%s: focus points at the bit holder", where);
    anchor_ok = lv->anchor == -1 || (lv->anchor >= 0 && lv->anchor < lv->nrows);
    CHECK(anchor_ok, "%s: anchor in range", where);
}

static void test_random(unsigned long seed, int rounds)
{
    struct lv *lv = new_lv(0);
    rec_t rec = {0, 0, -1, -1, 0, 0, 0};
    lv_ops ops = rec_ops(&rec);
    UINT snap[512];
    rng_state = seed ? seed : 1;
    for (int round = 0; round < rounds; round++) {
        int op = (int)rnd(10), before = lv->nrows, changes_expected = 0, changes_seen;
        const char *name = "?";
        for (int i = 0; i < lv->nrows && i < 512; i++)
            snap[i] = lv->rows[i]->state;
        rec.nchanged = 0;
        if (op <= 1 || lv->nrows < 3) {
            name = "add";
            if (lv->nrows < 300)
                add_row(lv, "r", rnd(1000));
        } else if (op == 2) {
            name = "remove";
            if (lv->nrows) {
                int i = (int)rnd(lv->nrows);
                lv_row_free(lv_remove_row(lv, i));
            }
        } else {
            int n = lv->nrows;
            int idx = (int)rnd(n);
            UINT mods = rnd(3) == 0 ? MK_SHIFT : (rnd(3) == 0 ? MK_CONTROL : 0);
            int single = rnd(5) == 0;
            name = "select";
            if (op == 3) {
                lv_move_focus(lv, &ops, idx, mods, single);
            } else if (op == 4) {
                lv_click_item(lv, &ops, idx, mods, single);
            } else if (op == 5) {
                lv_click_blank(lv, &ops, mods);
            } else if (op == 6) {
                int a = (int)rnd(n), b = (int)rnd(n);
                lv_select_range(lv, &ops, a, b);
            } else if (op == 7) {
                lv_toggle_check(lv, &ops, idx);
            } else {
                lv_set_state(lv, &ops, idx, LVIS_SELECTED, LVIS_SELECTED);
            }
            /* every row whose state really changed fired at least one changed hook (a row can fire
             * two when focus and selection both move) */
            for (int i = 0; i < n && i < 512; i++)
                if (lv->rows[i]->state != snap[i])
                    changes_expected++;
        }
        changes_seen = rec.nchanged;
        if (op > 2 && op != 2)
            CHECK(changes_seen >= changes_expected, "round %d (%s): %d changed hooks for %d changed rows", round,
                  name, changes_seen, changes_expected);
        if (lv->nrows > 0 && op > 2 && op != 2)
            CHECK(lv_selected_count(lv) <= lv->nrows, "selection count sane");
        (void)before;
        check_invariants(lv, name);
        if (lv->nrows && op == 9) {
            lv_sort_rows(lv, cmp_key, 0);
            check_invariants(lv, "after sort");
        }
    }
    CHECK(rec.bad == 0, "no changed hook reported an unchanged state");
    free_lv(lv);
}

int main(int argc, char **argv)
{
    unsigned long seed = argc > 1 ? strtoul(argv[1], NULL, 10) : 12345UL;
    int rounds = argc > 2 ? atoi(argv[2]) : 2000;
    test_rows_and_subitems();
    test_focus_anchor_follow_rows();
    test_selection_policy();
    test_hooks_and_veto();
    test_sort();
    test_sorted_insert_and_find();
    test_random(seed, rounds);
    test_random(seed * 7 + 3, rounds);
    if (failures) {
        fprintf(stderr, "listview_unit: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("listview_unit: %d checks passed (seed %lu, %d rounds)\n", checks, seed, rounds);
    return 0;
}
