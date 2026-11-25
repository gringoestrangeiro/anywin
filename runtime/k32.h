/* anywin runtime - internal interfaces of the kernel32 package (package A).
 * Shared between util.c, sync.c, kernel32.c, file.c, console.c, locale.c,
 * process.c.  Nothing here is part of the public Win32 surface. */
#ifndef AW_K32_H
#define AW_K32_H
#include "aw.h"
#include <pthread.h>
#include <sys/types.h>

/* ---- sync.c services ------------------------------------------------- */
aw_object *aw_object_new(int type);
int *aw_obj_inherit(aw_object *o);
void aw_obj_signal(aw_object *o);         /* set event-like object signaled + wake waiters */
int aw_obj_is_signaled(aw_object *o);
void aw_process_close(aw_object *o);      /* H_PROCESS release hook (frees ext) */
int aw_apc_run(DWORD tid);                /* deliver pending APCs / IO completions */
void aw_wait_signal(void);
void aw_wait_lock(void);
void aw_wait_unlock(void);

/* ---- process.c services ---------------------------------------------- */
int aw_is_self_process(HANDLE h);
DWORD aw_process_id(HANDLE h);
extern DWORD aw_self_pid;

/* ---- file.c services -------------------------------------------------- */
extern char aw_curdir_win[4096];
char *aw_full_path_win(const char *path); /* malloc'd canonical Windows path */
char *aw_module_file_name(void);          /* malloc'd Windows path of the ELF */
BOOL aw_handle_is_console(HANDLE h);
HANDLE aw_std_handle(int which);          /* 0 in, 1 out, 2 err */

/* ---- kernel32.c services --------------------------------------------- */
DWORD aw_system_dir(char *out, DWORD n, int windows);
void aw_init_environment(void);
extern char aw_exe_unix[4096], aw_exe_win[4096];
DWORD aw_muldiv(int a, int b, int c);

/* ---- console.c services ----------------------------------------------- */
int aw_console_mode(HANDLE h, int output);
void aw_console_init(void);

/* ---- file.c services (file.c is not in the tree yet; declared so callers compile) */
void aw_file_init(void);
void aw_std_init(void);

/* ---- locale.c services ------------------------------------------------ */
int aw_cp_to_utf16(UINT cp, DWORD flags, const char *src, int srclen, WCHAR *dst, int dstlen);
int aw_utf16_to_cp(UINT cp, DWORD flags, const WCHAR *src, int srclen, char *dst, int dstlen,
                   const char *default_char, BOOL *used_default);
DWORD aw_lcid_current(void);

#endif
