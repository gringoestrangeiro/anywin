/* anywin runtime - internal interfaces of the kernel32 package (S1).
 * Shared between sync.c and kernel32.c (util.c needs only aw.h).
 * Nothing here is part of the public Win32 surface. */
#ifndef AW_K32_H
#define AW_K32_H
#include "aw.h"
#include <pthread.h>
#include <sys/types.h>

/* ---- sync.c services (defined in sync.c) ----------------------------- */
aw_object *aw_object_new(int type);
int *aw_obj_inherit(aw_object *o);
int aw_obj_fd(aw_object *o);
void aw_obj_set_fd(aw_object *o, int fd);
int aw_obj_type(aw_object *o);
void **aw_obj_ext(aw_object *o);
char **aw_obj_path(aw_object *o);
int *aw_obj_delete_on_close(aw_object *o);
int *aw_obj_console(aw_object *o);
void aw_obj_signal(aw_object *o);         /* set event-like object signaled + wake waiters */
int aw_obj_is_signaled(aw_object *o);
void aw_wait_signal(void);
void aw_wait_lock(void);
void aw_wait_unlock(void);

/* ---- kernel32.c hooks called from sync.c ----------------------------- */
void aw_file_close(aw_object *o);         /* CloseHandle for H_FILE / H_FIND / H_MAPPING */
void aw_process_close(aw_object *o);      /* CloseHandle for H_PROCESS: frees *aw_obj_ext() */
int aw_process_poll(aw_object *o);        /* child reaping for waits; nonzero when signaled */
int aw_apc_run(DWORD tid);                /* run queued APCs; none can be queued in Stage 1 */

/* ---- kernel32.c services --------------------------------------------- */
extern char aw_exe_unix[4096], aw_exe_win[4096];
extern char aw_curdir_win[4096];          /* current directory as a Windows path, taken at startup */
void aw_init_environment(void);
HANDLE aw_std_handle(int which);          /* 0 in, 1 out, 2 err */
void *aw_process_heap(void);              /* identity of the process heap (PEB.ProcessHeap) */
void *aw_unhandled_filter(void);          /* last SetUnhandledExceptionFilter value (package C reads it) */

#endif
