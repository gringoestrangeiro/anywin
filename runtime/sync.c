/* kernel32: handle table, threads and synchronization */
#include "aw.h"
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct aw_object {
    int type;
    int refs;
    /* file */
    int fd;
    int console;
    char *path;
    int delete_on_close;
    /* waitable */
    int signaled;
    int manual_reset;
    DWORD owner;
    int recursion;
    LONG count, maxcount;
    /* thread */
    pthread_t thread;
    DWORD tid;
    DWORD exit_code;
    int finished;
    int suspended;
    THREADPROC proc;
    void *arg;
    /* other */
    void *ext;
    char *name;
};

#define MAX_HANDLES 65536
static aw_object *handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;

HANDLE aw_handle_alloc(int type, void *obj)
{
    aw_object *o = obj;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->fd = -1;
    }
    o->type = type;
    o->refs = 1;
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = o;
            pthread_mutex_unlock(&handle_lock);
            return (HANDLE)(uintptr_t)(i * 4);
        }
    pthread_mutex_unlock(&handle_lock);
    return NULL;
}

aw_object *aw_handle_object(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v & 3 || v / 4 >= MAX_HANDLES)
        return NULL;
    return handles[v / 4];
}

void *aw_handle_get(HANDLE h, int type)
{
    aw_object *o = aw_handle_object(h);
    if (!o || (type && o->type != type))
        return NULL;
    return o;
}

aw_object *aw_object_new(int type)
{
    aw_object *o = calloc(1, sizeof *o);
    o->type = type;
    o->fd = -1;
    return o;
}

int aw_handle_fd(HANDLE h)
{
    aw_object *o = aw_handle_object(h);
    return o && o->type == H_FILE ? o->fd : -1;
}

/* accessors used by file.c */
int aw_obj_fd(aw_object *o) { return o->fd; }
void aw_obj_set_fd(aw_object *o, int fd) { o->fd = fd; }
int aw_obj_type(aw_object *o) { return o->type; }
void **aw_obj_ext(aw_object *o) { return &o->ext; }
char **aw_obj_path(aw_object *o) { return &o->path; }
int *aw_obj_delete_on_close(aw_object *o) { return &o->delete_on_close; }
int *aw_obj_console(aw_object *o) { return &o->console; }

extern void aw_file_close(aw_object *o);

API(kernel32, BOOL, CloseHandle, (HANDLE h))
{
    uintptr_t v = (uintptr_t)h;
    if ((intptr_t)h == -1 || (intptr_t)h == -2)
        return TRUE;
    pthread_mutex_lock(&handle_lock);
    aw_object *o = (v & 3 || v / 4 >= MAX_HANDLES) ? NULL : handles[v / 4];
    if (!o) {
        pthread_mutex_unlock(&handle_lock);
        aw_set_last_error(6);
        return FALSE;
    }
    handles[v / 4] = NULL;
    int last = --o->refs == 0;
    pthread_mutex_unlock(&handle_lock);
    if (last) {
        if (o->type == H_FILE || o->type == H_FIND || o->type == H_MAPPING)
            aw_file_close(o);
        if (o->type == H_THREAD && o->finished)
            pthread_detach(o->thread);
        else if (o->type != H_THREAD) {
            free(o->name);
            free(o);
        }
    }
    return TRUE;
}

API(kernel32, BOOL, DuplicateHandle,
    (HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD options))
{
    aw_object *o;
    if ((intptr_t)src == -2) {
        extern HANDLE aw_current_thread_handle(void);
        src = aw_current_thread_handle();
    }
    if ((intptr_t)src == -1) {
        if (dst)
            *dst = src;
        return TRUE;
    }
    o = aw_handle_object(src);
    if (!o) {
        aw_set_last_error(6);
        return FALSE;
    }
    pthread_mutex_lock(&handle_lock);
    o->refs++;
    pthread_mutex_unlock(&handle_lock);
    HANDLE h = NULL;
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = o;
            h = (HANDLE)(uintptr_t)(i * 4);
            break;
        }
    pthread_mutex_unlock(&handle_lock);
    if (dst)
        *dst = h;
    if (options & 1) /* DUPLICATE_CLOSE_SOURCE */
        __aw_kernel32_CloseHandle(src);
    return TRUE;
}

/* ---- threads ------------------------------------------------------------- */
static __thread aw_object *self_thread;
static __thread HANDLE self_thread_handle;

extern void aw_thread_detach(void);
extern void aw_fls_thread_exit(void);

static void signal_all(void)
{
    pthread_cond_broadcast(&wait_cond);
}

static void *thread_start(void *p)
{
    aw_object *o = p;
    pthread_attr_t attr;
    void *stackaddr = NULL;
    size_t stacksize = 0;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
    aw_teb_create((uint8_t *)stackaddr + stacksize, stacksize);
    self_thread = o;
    pthread_mutex_lock(&wait_lock);
    o->tid = aw_current_tid();
    while (o->suspended)
        pthread_cond_wait(&wait_cond, &wait_lock);
    pthread_mutex_unlock(&wait_lock);
    aw_thread_attach();
    DWORD r = o->proc(o->arg);
    extern void WINAPI __aw_kernel32_ExitThread(DWORD);
    __aw_kernel32_ExitThread(r);
    return NULL;
}

HANDLE aw_create_thread(THREADPROC proc, void *arg, size_t stack, DWORD flags, DWORD *tid)
{
    aw_object *o = aw_object_new(H_THREAD);
    o->proc = proc;
    o->arg = arg;
    o->suspended = (flags & 4) != 0; /* CREATE_SUSPENDED */
    o->refs = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack < (8u << 20))
        stack = 8u << 20;
    pthread_attr_setstacksize(&attr, stack);
    HANDLE h = aw_handle_alloc(H_THREAD, o);
    o->refs = 2; /* handle + running thread */
    if (pthread_create(&o->thread, &attr, thread_start, o)) {
        pthread_attr_destroy(&attr);
        aw_set_last_error(8);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    /* wait for tid */
    if (tid) {
        for (int i = 0; i < 100000; i++) {
            pthread_mutex_lock(&wait_lock);
            DWORD t = o->tid;
            pthread_mutex_unlock(&wait_lock);
            if (t) {
                *tid = t;
                break;
            }
            sched_yield();
        }
    }
    return h;
}

API(kernel32, HANDLE, CreateThread,
    (void *sa, SIZE_T stack, THREADPROC proc, void *arg, DWORD flags, DWORD *tid))
{
    return aw_create_thread(proc, arg, stack, flags, tid);
}

API(kernel32, void, ExitThread, (DWORD code))
{
    aw_fls_thread_exit();
    aw_thread_detach();
    aw_object *o = self_thread;
    if (o) {
        pthread_mutex_lock(&wait_lock);
        o->exit_code = code;
        o->finished = 1;
        signal_all();
        pthread_mutex_unlock(&wait_lock);
    }
    pthread_exit(NULL);
}

API(kernel32, DWORD, ResumeThread, (HANDLE h))
{
    aw_object *o = aw_handle_get(h, H_THREAD);
    if (!o)
        return (DWORD)-1;
    pthread_mutex_lock(&wait_lock);
    DWORD prev = o->suspended;
    o->suspended = 0;
    signal_all();
    pthread_mutex_unlock(&wait_lock);
    return prev;
}

API(kernel32, DWORD, SuspendThread, (HANDLE h)) { return 0; }

API(kernel32, BOOL, GetExitCodeThread, (HANDLE h, DWORD *code))
{
    aw_object *o = aw_handle_get(h, H_THREAD);
    if (!o) {
        if (code)
            *code = 259;
        return h == (HANDLE)-2;
    }
    pthread_mutex_lock(&wait_lock);
    *code = o->finished ? o->exit_code : 259; /* STILL_ACTIVE */
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}

API(kernel32, BOOL, TerminateThread, (HANDLE h, DWORD code))
{
    aw_object *o = aw_handle_get(h, H_THREAD);
    if (!o)
        return FALSE;
    pthread_cancel(o->thread);
    pthread_mutex_lock(&wait_lock);
    o->exit_code = code;
    o->finished = 1;
    signal_all();
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}

HANDLE aw_current_thread_handle(void)
{
    if (!self_thread_handle) {
        if (!self_thread) {
            self_thread = aw_object_new(H_THREAD);
            self_thread->thread = pthread_self();
            self_thread->tid = aw_current_tid();
            self_thread->refs = 1;
        }
        self_thread->refs++;
        self_thread_handle = aw_handle_alloc(H_THREAD, self_thread);
        self_thread->refs = 2;
    }
    return self_thread_handle;
}

API(kernel32, HANDLE, GetCurrentThread, (void)) { return (HANDLE)(intptr_t)-2; }
API(kernel32, DWORD, GetCurrentThreadId, (void)) { return aw_current_tid(); }
API(kernel32, DWORD, GetThreadId, (HANDLE h))
{
    if (h == (HANDLE)-2)
        return aw_current_tid();
    aw_object *o = aw_handle_get(h, H_THREAD);
    return o ? o->tid : 0;
}
API(kernel32, BOOL, SetThreadPriority, (HANDLE h, int p)) { return TRUE; }
API(kernel32, int, GetThreadPriority, (HANDLE h)) { return 0; }
API(kernel32, BOOL, SetThreadPriorityBoost, (HANDLE h, BOOL b)) { return TRUE; }
API(kernel32, DWORD_PTR, SetThreadAffinityMask, (HANDLE h, DWORD_PTR m)) { return 1; }
API(kernel32, HRESULT, SetThreadDescription, (HANDLE h, const WCHAR *d)) { return 0; }
API(kernel32, BOOL, SwitchToThread, (void)) { sched_yield(); return TRUE; }
API(kernel32, DWORD, GetCurrentProcessorNumber, (void)) { return (DWORD)sched_getcpu(); }

/* ---- waitable objects ------------------------------------------------------ */
static HANDLE find_named(int type, const char *name)
{
    if (!name)
        return NULL;
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (handles[i] && handles[i]->type == type && handles[i]->name && !strcmp(handles[i]->name, name)) {
            handles[i]->refs++;
            aw_object *o = handles[i];
            pthread_mutex_unlock(&handle_lock);
            HANDLE h = NULL;
            pthread_mutex_lock(&handle_lock);
            for (int j = 1; j < MAX_HANDLES; j++)
                if (!handles[j]) {
                    handles[j] = o;
                    h = (HANDLE)(uintptr_t)(j * 4);
                    break;
                }
            pthread_mutex_unlock(&handle_lock);
            return h;
        }
    pthread_mutex_unlock(&handle_lock);
    return NULL;
}

static HANDLE create_event(BOOL manual, BOOL initial, const char *name)
{
    HANDLE h = find_named(H_EVENT, name);
    if (h) {
        aw_set_last_error(183);
        return h;
    }
    aw_object *o = aw_object_new(H_EVENT);
    o->manual_reset = manual;
    o->signaled = initial;
    o->name = name ? strdup(name) : NULL;
    aw_set_last_error(0);
    return aw_handle_alloc(H_EVENT, o);
}

API(kernel32, HANDLE, CreateEventA, (void *sa, BOOL manual, BOOL initial, const char *name))
{
    return create_event(manual, initial, name);
}
API(kernel32, HANDLE, CreateEventW, (void *sa, BOOL manual, BOOL initial, const WCHAR *name))
{
    char *n = aw_w2u(name, -1);
    HANDLE h = create_event(manual, initial, n);
    free(n);
    return h;
}
API(kernel32, HANDLE, CreateEventExA, (void *sa, const char *name, DWORD flags, DWORD access))
{
    return create_event(flags & 1, (flags & 2) != 0, name);
}
API(kernel32, HANDLE, CreateEventExW, (void *sa, const WCHAR *name, DWORD flags, DWORD access))
{
    char *n = aw_w2u(name, -1);
    HANDLE h = create_event(flags & 1, (flags & 2) != 0, n);
    free(n);
    return h;
}
API(kernel32, HANDLE, OpenEventA, (DWORD access, BOOL inherit, const char *name))
{
    HANDLE h = find_named(H_EVENT, name);
    if (!h)
        aw_set_last_error(2);
    return h;
}

API(kernel32, BOOL, SetEvent, (HANDLE h))
{
    aw_object *o = aw_handle_get(h, H_EVENT);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&wait_lock);
    o->signaled = 1;
    signal_all();
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}
API(kernel32, BOOL, ResetEvent, (HANDLE h))
{
    aw_object *o = aw_handle_get(h, H_EVENT);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&wait_lock);
    o->signaled = 0;
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}
API(kernel32, BOOL, PulseEvent, (HANDLE h))
{
    __aw_kernel32_SetEvent(h);
    sched_yield();
    return __aw_kernel32_ResetEvent(h);
}

static HANDLE create_mutex(BOOL owned, const char *name)
{
    HANDLE h = find_named(H_MUTEX, name);
    if (h) {
        aw_set_last_error(183);
        return h;
    }
    aw_object *o = aw_object_new(H_MUTEX);
    if (owned) {
        o->owner = aw_current_tid();
        o->recursion = 1;
    }
    o->name = name ? strdup(name) : NULL;
    aw_set_last_error(0);
    return aw_handle_alloc(H_MUTEX, o);
}
API(kernel32, HANDLE, CreateMutexA, (void *sa, BOOL owned, const char *name)) { return create_mutex(owned, name); }
API(kernel32, HANDLE, CreateMutexW, (void *sa, BOOL owned, const WCHAR *name))
{
    char *n = aw_w2u(name, -1);
    HANDLE h = create_mutex(owned, n);
    free(n);
    return h;
}
API(kernel32, HANDLE, CreateMutexExW, (void *sa, const WCHAR *name, DWORD flags, DWORD access))
{
    char *n = aw_w2u(name, -1);
    HANDLE h = create_mutex(flags & 1, n);
    free(n);
    return h;
}
API(kernel32, HANDLE, OpenMutexA, (DWORD access, BOOL inherit, const char *name))
{
    HANDLE h = find_named(H_MUTEX, name);
    if (!h)
        aw_set_last_error(2);
    return h;
}
API(kernel32, BOOL, ReleaseMutex, (HANDLE h))
{
    aw_object *o = aw_handle_get(h, H_MUTEX);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&wait_lock);
    if (o->owner != aw_current_tid()) {
        pthread_mutex_unlock(&wait_lock);
        aw_set_last_error(288);
        return FALSE;
    }
    if (--o->recursion == 0) {
        o->owner = 0;
        signal_all();
    }
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}

static HANDLE create_sem(LONG initial, LONG max, const char *name)
{
    HANDLE h = find_named(H_SEM, name);
    if (h) {
        aw_set_last_error(183);
        return h;
    }
    aw_object *o = aw_object_new(H_SEM);
    o->count = initial;
    o->maxcount = max;
    o->name = name ? strdup(name) : NULL;
    aw_set_last_error(0);
    return aw_handle_alloc(H_SEM, o);
}
API(kernel32, HANDLE, CreateSemaphoreA, (void *sa, LONG initial, LONG max, const char *name))
{
    return create_sem(initial, max, name);
}
API(kernel32, HANDLE, CreateSemaphoreW, (void *sa, LONG initial, LONG max, const WCHAR *name))
{
    char *n = aw_w2u(name, -1);
    HANDLE h = create_sem(initial, max, n);
    free(n);
    return h;
}
API(kernel32, HANDLE, CreateSemaphoreExW,
    (void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access))
{
    return __aw_kernel32_CreateSemaphoreW(sa, initial, max, name);
}
API(kernel32, BOOL, ReleaseSemaphore, (HANDLE h, LONG n, LONG *prev))
{
    aw_object *o = aw_handle_get(h, H_SEM);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&wait_lock);
    if (prev)
        *prev = o->count;
    if (o->count + n > o->maxcount) {
        pthread_mutex_unlock(&wait_lock);
        aw_set_last_error(298);
        return FALSE;
    }
    o->count += n;
    signal_all();
    pthread_mutex_unlock(&wait_lock);
    return TRUE;
}

/* returns 1 if object is signaled (and consumes it when acquire) */
static int check_obj(aw_object *o, int acquire)
{
    DWORD me = aw_current_tid();
    switch (o->type) {
    case H_EVENT:
        if (!o->signaled)
            return 0;
        if (acquire && !o->manual_reset)
            o->signaled = 0;
        return 1;
    case H_MUTEX:
        if (o->owner && o->owner != me)
            return 0;
        if (acquire) {
            o->owner = me;
            o->recursion++;
        }
        return 1;
    case H_SEM:
        if (o->count <= 0)
            return 0;
        if (acquire)
            o->count--;
        return 1;
    case H_THREAD:
        return o->finished;
    case H_PROCESS:
        return o->finished;
    default:
        return 1; /* files, console: always signaled */
    }
}

extern int aw_process_poll(aw_object *o); /* for child processes */

static void deadline_from(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

DWORD aw_wait_multiple(DWORD n, const HANDLE *hs, BOOL all, DWORD ms)
{
    aw_object *objs[64];
    if (n == 0 || n > 64) {
        aw_set_last_error(87);
        return (DWORD)-1;
    }
    for (DWORD i = 0; i < n; i++) {
        HANDLE h = hs[i];
        if (h == (HANDLE)-2)
            h = aw_current_thread_handle();
        objs[i] = aw_handle_object(h);
        if (!objs[i]) {
            aw_set_last_error(6);
            return (DWORD)-1;
        }
    }
    struct timespec dl;
    if (ms != 0xffffffff)
        deadline_from(ms, &dl);
    pthread_mutex_lock(&wait_lock);
    for (;;) {
        for (DWORD i = 0; i < n; i++)
            if (objs[i]->type == H_PROCESS && !objs[i]->finished)
                aw_process_poll(objs[i]);
        if (all) {
            DWORD ok = 0;
            for (DWORD i = 0; i < n; i++)
                ok += check_obj(objs[i], 0);
            if (ok == n) {
                for (DWORD i = 0; i < n; i++)
                    check_obj(objs[i], 1);
                pthread_mutex_unlock(&wait_lock);
                return 0;
            }
        } else {
            for (DWORD i = 0; i < n; i++)
                if (check_obj(objs[i], 1)) {
                    pthread_mutex_unlock(&wait_lock);
                    return i;
                }
        }
        if (ms == 0)
            break;
        int has_proc = 0;
        for (DWORD i = 0; i < n; i++)
            has_proc |= objs[i]->type == H_PROCESS;
        if (has_proc) {
            /* processes are polled; wake up periodically */
            struct timespec t;
            deadline_from(10, &t);
            if (ms != 0xffffffff && (t.tv_sec > dl.tv_sec || (t.tv_sec == dl.tv_sec && t.tv_nsec > dl.tv_nsec)))
                t = dl;
            pthread_cond_timedwait(&wait_cond, &wait_lock, &t);
            if (ms != 0xffffffff) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec))
                    break;
            }
        } else if (ms == 0xffffffff) {
            pthread_cond_wait(&wait_cond, &wait_lock);
        } else if (pthread_cond_timedwait(&wait_cond, &wait_lock, &dl) == ETIMEDOUT) {
            /* final check */
            for (DWORD i = 0; i < n && !all; i++)
                if (check_obj(objs[i], 1)) {
                    pthread_mutex_unlock(&wait_lock);
                    return i;
                }
            break;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return 258; /* WAIT_TIMEOUT */
}

void aw_wait_signal(void)
{
    pthread_mutex_lock(&wait_lock);
    signal_all();
    pthread_mutex_unlock(&wait_lock);
}
void aw_wait_lock(void) { pthread_mutex_lock(&wait_lock); }
void aw_wait_unlock(void) { signal_all(); pthread_mutex_unlock(&wait_lock); }
void aw_obj_set_finished(aw_object *o, DWORD code)
{
    o->finished = 1;
    o->exit_code = code;
}
DWORD aw_obj_exit_code(aw_object *o) { return o->exit_code; }
int aw_obj_finished(aw_object *o) { return o->finished; }

API(kernel32, DWORD, WaitForSingleObject, (HANDLE h, DWORD ms)) { return aw_wait_multiple(1, &h, FALSE, ms); }
API(kernel32, DWORD, WaitForSingleObjectEx, (HANDLE h, DWORD ms, BOOL alertable))
{
    return aw_wait_multiple(1, &h, FALSE, ms);
}
API(kernel32, DWORD, WaitForMultipleObjects, (DWORD n, const HANDLE *h, BOOL all, DWORD ms))
{
    return aw_wait_multiple(n, h, all, ms);
}
API(kernel32, DWORD, WaitForMultipleObjectsEx, (DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable))
{
    return aw_wait_multiple(n, h, all, ms);
}
API(kernel32, DWORD, SignalObjectAndWait, (HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable))
{
    aw_object *o = aw_handle_object(sig);
    if (o && o->type == H_EVENT)
        __aw_kernel32_SetEvent(sig);
    else if (o && o->type == H_MUTEX)
        __aw_kernel32_ReleaseMutex(sig);
    else if (o && o->type == H_SEM)
        __aw_kernel32_ReleaseSemaphore(sig, 1, NULL);
    return aw_wait_multiple(1, &wait, FALSE, ms);
}

/* ---- futex based primitives ------------------------------------------------ */
static void futex_wait(volatile int *addr, int val, const struct timespec *rel)
{
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, rel, NULL, 0);
}
static void futex_wake(volatile int *addr, int n) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0); }

/* CRITICAL_SECTION layout (x64, 40 bytes):
 *  0 DebugInfo, 8 LockCount, 12 RecursionCount, 16 OwningThread, 24 LockSemaphore, 32 SpinCount
 * We use LockSemaphore's low 32 bits as futex word. */
typedef struct {
    void *DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION;

static volatile int *cs_word(CRITICAL_SECTION *cs) { return (volatile int *)&cs->LockSemaphore; }

API(kernel32, void, InitializeCriticalSection, (CRITICAL_SECTION * cs))
{
    memset(cs, 0, sizeof *cs);
    cs->LockCount = -1;
}
API(kernel32, BOOL, InitializeCriticalSectionAndSpinCount, (CRITICAL_SECTION * cs, DWORD spin))
{
    __aw_kernel32_InitializeCriticalSection(cs);
    cs->SpinCount = spin;
    return TRUE;
}
API(kernel32, BOOL, InitializeCriticalSectionEx, (CRITICAL_SECTION * cs, DWORD spin, DWORD flags))
{
    return __aw_kernel32_InitializeCriticalSectionAndSpinCount(cs, spin);
}
API(kernel32, DWORD, SetCriticalSectionSpinCount, (CRITICAL_SECTION * cs, DWORD spin))
{
    DWORD old = (DWORD)cs->SpinCount;
    cs->SpinCount = spin;
    return old;
}
API(kernel32, void, DeleteCriticalSection, (CRITICAL_SECTION * cs)) {}

API(kernel32, void, EnterCriticalSection, (CRITICAL_SECTION * cs))
{
    DWORD me = aw_current_tid();
    if ((DWORD)(uintptr_t)cs->OwningThread == me) {
        cs->RecursionCount++;
        return;
    }
    volatile int *w = cs_word(cs);
    int c = __sync_val_compare_and_swap(w, 0, 1);
    if (c != 0) {
        if (c != 2)
            c = __sync_lock_test_and_set(w, 2);
        while (c != 0) {
            futex_wait(w, 2, NULL);
            c = __sync_lock_test_and_set(w, 2);
        }
    }
    cs->OwningThread = (HANDLE)(uintptr_t)me;
    cs->RecursionCount = 1;
    cs->LockCount = 0;
}
API(kernel32, BOOL, TryEnterCriticalSection, (CRITICAL_SECTION * cs))
{
    DWORD me = aw_current_tid();
    if ((DWORD)(uintptr_t)cs->OwningThread == me) {
        cs->RecursionCount++;
        return TRUE;
    }
    if (__sync_val_compare_and_swap(cs_word(cs), 0, 1) != 0)
        return FALSE;
    cs->OwningThread = (HANDLE)(uintptr_t)me;
    cs->RecursionCount = 1;
    cs->LockCount = 0;
    return TRUE;
}
API(kernel32, void, LeaveCriticalSection, (CRITICAL_SECTION * cs))
{
    if (--cs->RecursionCount > 0)
        return;
    cs->OwningThread = 0;
    cs->LockCount = -1;
    volatile int *w = cs_word(cs);
    if (__sync_fetch_and_sub(w, 1) != 1) {
        *w = 0;
        futex_wake(w, 1);
    }
}

/* SRW locks: one pointer-sized word.  Simple implementation:
 * bit 0 = exclusive held, bits 1.. = reader count*2 ; waiters spin on futex */
typedef struct { volatile int word; volatile int seq; } SRW;

API(kernel32, void, InitializeSRWLock, (void **l)) { *l = NULL; }
API(kernel32, void, AcquireSRWLockExclusive, (void **l))
{
    SRW *s = (SRW *)l;
    for (;;) {
        if (__sync_bool_compare_and_swap(&s->word, 0, 1))
            return;
        int seq = s->seq;
        if (s->word != 0)
            futex_wait(&s->seq, seq, NULL);
    }
}
API(kernel32, BYTE, TryAcquireSRWLockExclusive, (void **l))
{
    SRW *s = (SRW *)l;
    return __sync_bool_compare_and_swap(&s->word, 0, 1);
}
API(kernel32, void, ReleaseSRWLockExclusive, (void **l))
{
    SRW *s = (SRW *)l;
    s->word = 0;
    __sync_fetch_and_add(&s->seq, 1);
    futex_wake(&s->seq, 0x7fffffff);
}
API(kernel32, void, AcquireSRWLockShared, (void **l))
{
    SRW *s = (SRW *)l;
    for (;;) {
        int w = s->word;
        if (!(w & 1) && __sync_bool_compare_and_swap(&s->word, w, w + 2))
            return;
        int seq = s->seq;
        if (s->word & 1)
            futex_wait(&s->seq, seq, NULL);
    }
}
API(kernel32, BYTE, TryAcquireSRWLockShared, (void **l))
{
    SRW *s = (SRW *)l;
    int w = s->word;
    return !(w & 1) && __sync_bool_compare_and_swap(&s->word, w, w + 2);
}
API(kernel32, void, ReleaseSRWLockShared, (void **l))
{
    SRW *s = (SRW *)l;
    if (__sync_sub_and_fetch(&s->word, 2) == 0) {
        __sync_fetch_and_add(&s->seq, 1);
        futex_wake(&s->seq, 0x7fffffff);
    }
}

/* condition variables: pointer-sized word used as sequence counter */
API(kernel32, void, InitializeConditionVariable, (void **cv)) { *cv = NULL; }
API(kernel32, void, WakeConditionVariable, (void **cv))
{
    __sync_fetch_and_add((volatile int *)cv, 1);
    futex_wake((volatile int *)cv, 1);
}
API(kernel32, void, WakeAllConditionVariable, (void **cv))
{
    __sync_fetch_and_add((volatile int *)cv, 1);
    futex_wake((volatile int *)cv, 0x7fffffff);
}
static BOOL cv_wait(void **cv, DWORD ms, void (*unlock)(void *), void (*lock)(void *), void *arg)
{
    int seq = *(volatile int *)cv;
    unlock(arg);
    struct timespec ts, *pts = NULL;
    if (ms != 0xffffffff) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000;
        pts = &ts;
    }
    long r = syscall(SYS_futex, cv, FUTEX_WAIT_PRIVATE, seq, pts, NULL, 0);
    int timedout = r == -1 && errno == ETIMEDOUT;
    lock(arg);
    if (timedout) {
        aw_set_last_error(1460); /* ERROR_TIMEOUT */
        return FALSE;
    }
    return TRUE;
}
static void cs_unlock(void *a) { __aw_kernel32_LeaveCriticalSection(a); }
static void cs_lock(void *a) { __aw_kernel32_EnterCriticalSection(a); }
static void srw_unlock_x(void *a) { __aw_kernel32_ReleaseSRWLockExclusive(a); }
static void srw_lock_x(void *a) { __aw_kernel32_AcquireSRWLockExclusive(a); }
static void srw_unlock_s(void *a) { __aw_kernel32_ReleaseSRWLockShared(a); }
static void srw_lock_s(void *a) { __aw_kernel32_AcquireSRWLockShared(a); }

API(kernel32, BOOL, SleepConditionVariableCS, (void **cv, CRITICAL_SECTION *cs, DWORD ms))
{
    int rec = cs->RecursionCount;
    cs->RecursionCount = 1;
    BOOL r = cv_wait(cv, ms, cs_unlock, cs_lock, cs);
    cs->RecursionCount = rec;
    return r;
}
API(kernel32, BOOL, SleepConditionVariableSRW, (void **cv, void **srw, DWORD ms, ULONG flags))
{
    if (flags & 1) /* CONDITION_VARIABLE_LOCKMODE_SHARED */
        return cv_wait(cv, ms, srw_unlock_s, srw_lock_s, srw);
    return cv_wait(cv, ms, srw_unlock_x, srw_lock_x, srw);
}

/* WaitOnAddress family */
API(kernel32, BOOL, WaitOnAddress, (volatile void *addr, void *cmp, SIZE_T size, DWORD ms))
{
    for (;;) {
        if (memcmp((const void *)addr, cmp, size))
            return TRUE;
        /* futex on the containing aligned int */
        volatile int *w = (volatile int *)((uintptr_t)addr & ~3ul);
        int cur = *w;
        if (memcmp((const void *)addr, cmp, size))
            return TRUE;
        struct timespec ts = {0, 1000000}, *pts = NULL;
        if (ms != 0xffffffff) {
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = (long)(ms % 1000) * 1000000;
            pts = &ts;
        }
        long r = syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, cur, pts, NULL, 0);
        if (r == -1 && errno == ETIMEDOUT) {
            aw_set_last_error(1460);
            return FALSE;
        }
        if (ms != 0xffffffff)
            return TRUE;
    }
}
API(kernel32, void, WakeByAddressSingle, (void *addr)) { futex_wake((volatile int *)((uintptr_t)addr & ~3ul), 0x7fffffff); }
API(kernel32, void, WakeByAddressAll, (void *addr)) { futex_wake((volatile int *)((uintptr_t)addr & ~3ul), 0x7fffffff); }

/* InitOnce */
API(kernel32, BOOL, InitOnceBeginInitialize, (void **once, DWORD flags, BOOL *pending, void **ctx))
{
    volatile int *w = (volatile int *)once;
    for (;;) {
        int v = *w;
        if (v == 2) {
            *pending = FALSE;
            return TRUE;
        }
        if (v == 0 && __sync_bool_compare_and_swap(w, 0, 1)) {
            *pending = TRUE;
            return TRUE;
        }
        if (flags & 2) { /* INIT_ONCE_CHECK_ONLY */
            aw_set_last_error(996);
            return FALSE;
        }
        futex_wait(w, 1, NULL);
    }
}
API(kernel32, BOOL, InitOnceComplete, (void **once, DWORD flags, void *ctx))
{
    volatile int *w = (volatile int *)once;
    *w = (flags & 4) ? 0 : 2; /* INIT_ONCE_INIT_FAILED */
    futex_wake(w, 0x7fffffff);
    return TRUE;
}
typedef BOOL(AWCALL *INITONCEFN)(void **, void *, void **);
API(kernel32, BOOL, InitOnceExecuteOnce, (void **once, INITONCEFN fn, void *param, void **ctx))
{
    BOOL pending;
    if (!__aw_kernel32_InitOnceBeginInitialize(once, 0, &pending, ctx))
        return FALSE;
    if (!pending)
        return TRUE;
    BOOL ok = fn(once, param, ctx);
    __aw_kernel32_InitOnceComplete(once, ok ? 0 : 4, NULL);
    return ok;
}
API(kernel32, void, InitOnceInitialize, (void **once)) { *once = NULL; }

/* ---- TLS / FLS -------------------------------------------------------------- */
extern void **aw_teb_tls_slots(void);
extern void **aw_teb_tls_expansion(void);
static uint8_t tls_used[1088];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

API(kernel32, DWORD, TlsAlloc, (void))
{
    pthread_mutex_lock(&tls_lock);
    for (int i = 0; i < 1088; i++)
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            return i;
        }
    pthread_mutex_unlock(&tls_lock);
    return 0xffffffff;
}
API(kernel32, BOOL, TlsFree, (DWORD i))
{
    if (i >= 1088)
        return FALSE;
    tls_used[i] = 0;
    return TRUE;
}
static void **tls_slot(DWORD i)
{
    if (i < 64)
        return &aw_teb_tls_slots()[i];
    void **exp = aw_teb_tls_expansion();
    if (!*exp)
        *exp = calloc(1024, sizeof(void *));
    return &((void **)*exp)[i - 64];
}
API(kernel32, void *, TlsGetValue, (DWORD i))
{
    if (i >= 1088) {
        aw_set_last_error(87);
        return NULL;
    }
    aw_set_last_error(0);
    return *tls_slot(i);
}
API(kernel32, BOOL, TlsSetValue, (DWORD i, void *v))
{
    if (i >= 1088) {
        aw_set_last_error(87);
        return FALSE;
    }
    *tls_slot(i) = v;
    return TRUE;
}

typedef void(AWCALL *FLSCB)(void *);
static FLSCB fls_cb[1088];
static uint8_t fls_used[1088];
static __thread void *fls_vals[1088];
API(kernel32, DWORD, FlsAlloc, (FLSCB cb))
{
    pthread_mutex_lock(&tls_lock);
    for (int i = 1; i < 1088; i++)
        if (!fls_used[i]) {
            fls_used[i] = 1;
            fls_cb[i] = cb;
            pthread_mutex_unlock(&tls_lock);
            return i;
        }
    pthread_mutex_unlock(&tls_lock);
    return 0xffffffff;
}
API(kernel32, BOOL, FlsFree, (DWORD i))
{
    if (i >= 1088 || !fls_used[i])
        return FALSE;
    if (fls_cb[i] && fls_vals[i])
        fls_cb[i](fls_vals[i]);
    fls_used[i] = 0;
    fls_cb[i] = NULL;
    fls_vals[i] = NULL;
    return TRUE;
}
API(kernel32, void *, FlsGetValue, (DWORD i))
{
    if (i >= 1088)
        return NULL;
    aw_set_last_error(0);
    return fls_vals[i];
}
API(kernel32, BOOL, FlsSetValue, (DWORD i, void *v))
{
    if (i >= 1088)
        return FALSE;
    fls_vals[i] = v;
    return TRUE;
}
void aw_fls_thread_exit(void)
{
    for (int i = 1; i < 1088; i++)
        if (fls_used[i] && fls_cb[i] && fls_vals[i]) {
            void *v = fls_vals[i];
            fls_vals[i] = NULL;
            fls_cb[i](v);
        }
}

/* ---- Interlocked SList ------------------------------------------------------ */
typedef struct SLIST_ENTRY { struct SLIST_ENTRY *Next; } SLIST_ENTRY;
typedef struct { SLIST_ENTRY *head; uint64_t depth; } __attribute__((aligned(16))) SLIST_HEADER;
static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;
API(kernel32, void, InitializeSListHead, (SLIST_HEADER * h)) { memset(h, 0, sizeof *h); }
API(kernel32, SLIST_ENTRY *, InterlockedPushEntrySList, (SLIST_HEADER * h, SLIST_ENTRY *e))
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *old = h->head;
    e->Next = old;
    h->head = e;
    h->depth++;
    pthread_mutex_unlock(&slist_lock);
    return old;
}
API(kernel32, SLIST_ENTRY *, InterlockedPopEntrySList, (SLIST_HEADER * h))
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *e = h->head;
    if (e) {
        h->head = e->Next;
        h->depth--;
    }
    pthread_mutex_unlock(&slist_lock);
    return e;
}
API(kernel32, SLIST_ENTRY *, InterlockedFlushSList, (SLIST_HEADER * h))
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *e = h->head;
    h->head = NULL;
    h->depth = 0;
    pthread_mutex_unlock(&slist_lock);
    return e;
}
API(kernel32, USHORT, QueryDepthSList, (SLIST_HEADER * h)) { return (USHORT)h->depth; }
