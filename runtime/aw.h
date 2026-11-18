/* anywin runtime - internal common header */
#ifndef AW_H
#define AW_H
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define WINAPI __attribute__((ms_abi))
#define AWCALL __attribute__((ms_abi))

typedef int32_t BOOL, LONG, INT;
typedef uint32_t DWORD, UINT, ULONG;
typedef uint16_t WORD, USHORT, ATOM, WCHAR;
typedef uint8_t BYTE, UCHAR;
typedef int16_t SHORT;
typedef int64_t LONG_PTR, INT_PTR, LPARAM, LRESULT, LONGLONG;
typedef uint64_t ULONG_PTR, UINT_PTR, WPARAM, DWORD_PTR, SIZE_T, ULONGLONG, DWORD64;
typedef void *HANDLE, *HWND, *HINSTANCE, *HMODULE, *HDC, *HGDIOBJ, *HBRUSH, *HPEN, *HFONT, *HBITMAP, *HRGN,
    *HICON, *HCURSOR, *HMENU, *HKEY, *HPALETTE, *HRSRC, *HGLOBAL, *HLOCAL, *HMONITOR, *HACCEL, *HIMAGELIST,
    *HTHEME;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef DWORD COLORREF;
typedef LONG HRESULT;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

/* Windows calling convention callbacks */
typedef LRESULT(AWCALL *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR(AWCALL *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void(AWCALL *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef DWORD(AWCALL *THREADPROC)(LPVOID);
typedef void(AWCALL *VOIDPROC)(void);
typedef int(AWCALL *INTPROC)(void);

/* ---- export registry ------------------------------------------------- */
struct aw_export {
    const char *module;
    const char *name;
    void *addr;
};

#define AW_EXPORT_ENTRY(mod, nm, sym)                                                                     \
    static const struct aw_export __aw_exp_##mod##_##sym __attribute__((used, section("aw_exports"),       \
                                                                         aligned(8))) = {#mod, nm, (void *)&sym};

/* Define an exported API function: API(kernel32, DWORD, GetLastError, (void)) { ... } */
#define API(mod, ret, name, args)                                                                          \
    ret WINAPI __aw_##mod##_##name args;                                                                   \
    AW_EXPORT_ENTRY(mod, #name, __aw_##mod##_##name)                                                       \
    ret WINAPI __aw_##mod##_##name args

/* Exported variadic function (ms_abi varargs) */
#define APIV API

/* Exported function with a symbol name that isn't a C identifier */
#define API_ASM(mod, expname, ret, cname, args)                                                            \
    ret WINAPI cname args __asm__("__aw_" #mod "_" expname);                                               \
    static const struct aw_export __aw_exp_##cname __attribute__((used, section("aw_exports"), aligned(8))) = \
        {#mod, expname, (void *)&cname};                                                                   \
    ret WINAPI cname args

/* Exported data */
#define APIDATA(mod, type, name)                                                                           \
    extern type __aw_##mod##_##name;                                                                       \
    AW_EXPORT_ENTRY(mod, #name, __aw_##mod##_##name)                                                       \
    type __aw_##mod##_##name

/* Export an existing function under another module/name */
#define ALIAS(mod, name, target)                                                                           \
    extern __typeof(target) __aw_##mod##_##name __attribute__((alias(#target)));                           \
    AW_EXPORT_ENTRY(mod, #name, __aw_##mod##_##name)

/* ---- runtime internals ------------------------------------------------ */
extern char __aw_image_base[];
extern const uint64_t __aw_image_entry;
extern const int __aw_image_subsystem;

void *aw_find_export(const char *module, const char *name);
extern int aw_debug;
#define TRACE(...) do { if (aw_debug) aw_log(__VA_ARGS__); } while (0)
void aw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void aw_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* TEB / threads */
typedef struct aw_teb aw_teb;
aw_teb *aw_teb_current(void);
aw_teb *aw_teb_create(void *stack_base, size_t stack_size);
void aw_thread_attach(void);
void aw_set_last_error(DWORD e);
DWORD aw_get_last_error(void);
#define SetLastErrorInt(e) aw_set_last_error(e)
DWORD aw_errno_to_win(int e);
DWORD aw_current_tid(void);

/* strings */
size_t aw_wcslen(const WCHAR *s);
char *aw_w2u(const WCHAR *w, int len);       /* malloc'd utf-8, len -1 = nul terminated */
WCHAR *aw_u2w(const char *s, int len);       /* malloc'd utf-16 */
int aw_u2w_buf(const char *s, int len, WCHAR *out, int outlen); /* returns chars written (no nul) */
int aw_w2u_buf(const WCHAR *w, int len, char *out, int outlen);
#define IS_INTRESOURCE(p) (((uintptr_t)(p) >> 16) == 0)

/* paths */
char *aw_unix_path(const char *winpath);     /* malloc'd */
char *aw_unix_pathW(const WCHAR *winpath);
char *aw_win_path(const char *unixpath);     /* malloc'd */

/* kernel handles */
enum { H_NONE, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_PROCESS, H_FIND, H_MAPPING, H_CONSOLE_IN };
typedef struct aw_object aw_object;
HANDLE aw_handle_alloc(int type, void *obj);
void *aw_handle_get(HANDLE h, int type);
aw_object *aw_handle_object(HANDLE h);
int aw_handle_fd(HANDLE h);

/* time */
uint64_t aw_now_filetime(void);
uint32_t aw_tick_ms(void);

/* resources */
void *aw_find_resource(void *module, const void *type, const void *name, uint32_t *size);

/* GUI hooks used by kernel parts */
extern void (*aw_gui_atexit)(void);

#endif
