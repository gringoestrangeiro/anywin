#!/bin/bash
# Verify the runtime exports the internal hooks the plan requires.
# Usage: tests/check_hooks.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
rt=$build/libanywin.o
if [ ! -f "$rt" ]; then echo "check_hooks: $rt missing (run make)" >&2; exit 2; fi
syms="
aw_kernel32_init aw_msvcrt_init aw_seh_init aw_seh_signal
aw_file_close aw_process_poll aw_find_resource aw_now_filetime aw_tick_ms
aw_user_init aw_controls_register aw_dialogs_register aw_menus_register
aw_menubar_height aw_menubar_paint aw_menubar_track aw_menu_is_valid
aw_modal_loop aw_syscolor aw_icon_from_handle aw_tray_handle_event
surface_new surface_wrap surface_free surface_resize surface_mark_dirty
rgn_init rgn_free rgn_clear rgn_copy rgn_set_rect rgn_union_rect rgn_subtract_rect
rgn_intersect_rect rgn_union rgn_subtract rgn_intersect rgn_offset rgn_is_empty
rgn_bbox rgn_contains_point rgn_intersects_rect
gdi_create_window_dc gdi_is_window_dc gdi_delete_window_dc gdi_dc_window
gdi_dc_font gdi_draw_argb gdi_bitmap_surface
plat_init plat_fd plat_window_create plat_window_destroy plat_window_show
plat_window_move plat_window_set_title plat_window_set_icon plat_window_set_resizable
plat_window_activate plat_window_minimize plat_window_maximize plat_window_get_pos
plat_put plat_flush plat_poll plat_wakeup plat_cursor_pos plat_screen_size
plat_set_cursor plat_grab plat_clipboard_set plat_clipboard_get
plat_tray_add plat_tray_remove plat_tray_draw plat_bell
font_open font_metrics font_glyph font_advance
"
defined=$(nm -g --defined-only "$rt" 2>/dev/null | awk '{print $NF}')
missing=0
for s in $syms; do
    if ! printf '%s\n' "$defined" | grep -qx "$s"; then
        echo "MISSING $s"
        missing=$((missing+1))
    fi
done
echo "-- $missing internal hooks missing"
[ $missing -eq 0 ]
