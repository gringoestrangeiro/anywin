/* anywin runtime - prototypes of the public Win32 entry points that the GDI / user32 / resource
 * files of this package call through their __aw_<module>_<Name> symbols.  Each prototype matches
 * the API() definition exactly; files that already include user32_int.h may repeat them. */
#ifndef AW_API_H
#define AW_API_H
#include "user32_int.h"

#define AWP_GDI(ret, name, args) extern ret WINAPI __aw_gdi32_##name args;
#define AWP_USER(ret, name, args) extern ret WINAPI __aw_user32_##name args;
#define AWP_KERNEL(ret, name, args) extern ret WINAPI __aw_kernel32_##name args;

AWP_GDI(HFONT, CreateFontA, (int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR))
AWP_GDI(HFONT, CreateFontW, (int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR))
AWP_GDI(BOOL, GetTextExtentPoint32A, (HDC, LPCSTR, int, SIZE *))
AWP_GDI(BOOL, ExtTextOutW, (HDC, int, int, UINT, const RECT *, LPCWSTR, UINT, const INT *))
AWP_GDI(HDC, CreateCompatibleDC, (HDC))
AWP_GDI(BOOL, DeleteDC, (HDC))
AWP_GDI(HGDIOBJ, SelectObject, (HDC, HGDIOBJ))
AWP_GDI(BOOL, BitBlt, (HDC, int, int, int, int, HDC, int, int, DWORD))
AWP_GDI(BOOL, PatBlt, (HDC, int, int, int, int, DWORD))
AWP_GDI(HGDIOBJ, GetStockObject, (int))

AWP_USER(HDC, GetDC, (HWND))
AWP_USER(int, ReleaseDC, (HWND, HDC))
AWP_USER(UINT_PTR, SetTimer, (HWND, UINT_PTR, UINT, TIMERPROC))
AWP_USER(BOOL, KillTimer, (HWND, UINT_PTR))
AWP_USER(int, GetSystemMetrics, (int))
AWP_USER(HMENU, CreatePopupMenu, (void))
AWP_USER(BOOL, AppendMenuA, (HMENU, UINT, UINT_PTR, LPCSTR))
AWP_USER(BOOL, DestroyMenu, (HMENU))
AWP_USER(BOOL, IsZoomed, (HWND))
AWP_USER(BOOL, IsIconic, (HWND))
AWP_USER(BOOL, IsWindow, (HWND))
AWP_USER(BOOL, GetWindowRect, (HWND, RECT *))
AWP_USER(HWND, GetParent, (HWND))
AWP_USER(BOOL, ScreenToClient, (HWND, POINT *))
AWP_USER(BOOL, SetWindowPos, (HWND, HWND, int, int, int, int, UINT))
AWP_USER(BOOL, ShowWindow, (HWND, int))
AWP_USER(LONG, GetWindowLongW, (HWND, int))
AWP_USER(BOOL, SystemParametersInfoW, (UINT, UINT, void *, UINT))
AWP_USER(BOOL, PeekMessageW, (MSG *, HWND, UINT, UINT, UINT))

AWP_GDI(BOOL, DeleteObject, (HGDIOBJ))

AWP_USER(LRESULT, SendMessageA, (HWND, UINT, WPARAM, LPARAM))
AWP_USER(LRESULT, SendMessageW, (HWND, UINT, WPARAM, LPARAM))
AWP_USER(LRESULT, DefWindowProcA, (HWND, UINT, WPARAM, LPARAM))
AWP_USER(LRESULT, DefWindowProcW, (HWND, UINT, WPARAM, LPARAM))
AWP_USER(LONG_PTR, GetWindowLongPtrW, (HWND, int))
AWP_USER(LONG_PTR, SetWindowLongPtrW, (HWND, int, LONG_PTR))
AWP_USER(int, GetWindowTextA, (HWND, LPSTR, int))
AWP_USER(int, GetWindowTextW, (HWND, LPWSTR, int))
AWP_USER(BOOL, SetWindowTextA, (HWND, LPCSTR))
AWP_USER(BOOL, SetWindowTextW, (HWND, LPCWSTR))
AWP_USER(int, GetClassNameW, (HWND, LPWSTR, int))
AWP_USER(HWND, CreateWindowExW, (DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID))
AWP_USER(ATOM, RegisterClassExW, (const WNDCLASSEXW *))
AWP_USER(BOOL, DestroyWindow, (HWND))
AWP_USER(BOOL, IsChild, (HWND, HWND))
AWP_USER(BOOL, IsWindowVisible, (HWND))
AWP_USER(BOOL, IsWindowEnabled, (HWND))
AWP_USER(HWND, GetWindow, (HWND, UINT))
AWP_USER(int, GetDlgCtrlID, (HWND))
AWP_USER(HWND, SetFocus, (HWND))
AWP_USER(HWND, GetFocus, (void))
AWP_USER(BOOL, ClientToScreen, (HWND, POINT *))
AWP_USER(BOOL, AdjustWindowRectEx, (RECT *, DWORD, BOOL, DWORD))
AWP_USER(BOOL, SetForegroundWindow, (HWND))
AWP_USER(BOOL, EnableWindow, (HWND, BOOL))
AWP_USER(BOOL, PostMessageW, (HWND, UINT, WPARAM, LPARAM))
AWP_USER(void, PostQuitMessage, (int))
AWP_USER(BOOL, GetMessageW, (MSG *, HWND, UINT, UINT))
AWP_USER(BOOL, TranslateMessage, (const MSG *))
AWP_USER(LRESULT, DispatchMessageW, (const MSG *))
AWP_USER(BOOL, IsDialogMessageW, (HWND, MSG *))
AWP_USER(SHORT, GetKeyState, (int))
AWP_USER(INT_PTR, DialogBoxIndirectParamA, (HINSTANCE, const void *, HWND, DLGPROC, LPARAM))
AWP_USER(HWND, CreateDialogIndirectParamA, (HINSTANCE, const void *, HWND, DLGPROC, LPARAM))

AWP_KERNEL(DWORD, WaitForSingleObject, (HANDLE, DWORD))
#endif
