/* Process startup: TEB/PEB emulation through %gs, PE TLS, entry point. */
#include "aw.h"
#include <asm/prctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

int aw_debug;
void (*aw_gui_atexit)(void);

void aw_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[anywin] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void aw_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[anywin] fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    _exit(3);
}

/* ---- export table ---------------------------------------------------- */
extern const struct aw_export __start_aw_exports[], __stop_aw_exports[];

void *aw_find_export(const char *module, const char *name)
{
    for (const struct aw_export *e = __start_aw_exports; e < __stop_aw_exports; e++)
        if (!strcmp(e->name, name) && (!module || !strcmp(e->module, module)))
            return e->addr;
    return NULL;
}

/* ---- unimplemented import trap ----------------------------------------- */
static int strict_mode;
uint64_t WINAPI aw_unimplemented_ms(const char *name)
{
    static const char *seen[256];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == name)
            return 0;
    if (nseen < 256)
        seen[nseen++] = name;
    fprintf(stderr, "[anywin] unimplemented function called: %s\n", name);
    if (strict_mode)
        _exit(3);
    return 0;
}
__asm__(".text\n.globl __aw_unimplemented\n__aw_unimplemented:\n"
        "\tmov %rax, %rcx\n\tjmp aw_unimplemented_ms\n");

/* ---- TEB / PEB --------------------------------------------------------- */
#define TEB_SIZE 0x2000
struct aw_teb {
    uint8_t raw[TEB_SIZE];
};
#define TEB_FIELD(teb, off, type) (*(type *)((teb)->raw + (off)))

static uint8_t peb[0x400] __attribute__((aligned(16)));
static uint8_t proc_params[0x400] __attribute__((aligned(16)));
static uint32_t tls_index_value;
static void *tls_template;
static size_t tls_template_size, tls_zero_size;
static const uint64_t *tls_callbacks;

aw_teb *aw_teb_current(void)
{
    aw_teb *t;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(t));
    return t;
}

static void set_gs(void *p)
{
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, p))
        aw_fatal("arch_prctl(ARCH_SET_GS) failed");
}

DWORD aw_current_tid(void) { return (DWORD)syscall(SYS_gettid); }

static void *alloc_tls_block(void)
{
    if (!tls_template_size && !tls_zero_size)
        return NULL;
    uint8_t *b = calloc(1, tls_template_size + tls_zero_size + 16);
    if (tls_template_size)
        memcpy(b, tls_template, tls_template_size);
    return b;
}

aw_teb *aw_teb_create(void *stack_base, size_t stack_size)
{
    aw_teb *t = aligned_alloc(4096, sizeof(aw_teb));
    memset(t, 0, sizeof *t);
    TEB_FIELD(t, 0x00, void *) = (void *)-1;                       /* ExceptionList */
    TEB_FIELD(t, 0x08, void *) = stack_base;                        /* StackBase */
    TEB_FIELD(t, 0x10, void *) = (uint8_t *)stack_base - stack_size; /* StackLimit */
    TEB_FIELD(t, 0x30, void *) = t;                                 /* Self */
    TEB_FIELD(t, 0x40, uint64_t) = (uint64_t)getpid();
    TEB_FIELD(t, 0x48, uint64_t) = aw_current_tid();
    void **tlsarr = calloc(64, sizeof(void *));
    tlsarr[tls_index_value] = alloc_tls_block();
    TEB_FIELD(t, 0x58, void **) = tlsarr; /* ThreadLocalStoragePointer */
    TEB_FIELD(t, 0x60, void *) = peb;     /* PEB */
    set_gs(t);
    return t;
}

void aw_set_last_error(DWORD e) { TEB_FIELD(aw_teb_current(), 0x68, DWORD) = e; }
DWORD aw_get_last_error(void) { return TEB_FIELD(aw_teb_current(), 0x68, DWORD); }

void **aw_teb_tls_slots(void) { return (void **)(aw_teb_current()->raw + 0x1480); }
void **aw_teb_tls_expansion(void) { return &TEB_FIELD(aw_teb_current(), 0x1780, void *); }

static void run_tls_callbacks(DWORD reason)
{
    if (!tls_callbacks)
        return;
    for (const uint64_t *cb = tls_callbacks; *cb; cb++)
        ((void(AWCALL *)(void *, DWORD, void *))(uintptr_t)*cb)(__aw_image_base, reason, NULL);
}

void aw_thread_attach(void) { run_tls_callbacks(2 /* DLL_THREAD_ATTACH */); }
void aw_thread_detach(void) { run_tls_callbacks(3 /* DLL_THREAD_DETACH */); }

static void init_tls_directory(void)
{
    uint8_t *base = (uint8_t *)__aw_image_base;
    uint32_t pe = *(uint32_t *)(base + 0x3c);
    uint8_t *opt = base + pe + 24;
    uint32_t ndirs = *(uint32_t *)(opt + 108);
    if (ndirs <= 9)
        return;
    uint32_t rva = *(uint32_t *)(opt + 112 + 9 * 8);
    if (!rva)
        return;
    uint64_t *dir = (uint64_t *)(base + rva);
    uint64_t start = dir[0], end = dir[1], idx = dir[2], cbs = dir[3];
    uint32_t zero = *(uint32_t *)(dir + 4);
    tls_template = (void *)start;
    tls_template_size = end - start;
    tls_zero_size = zero;
    if (idx)
        *(uint32_t *)idx = tls_index_value = 0;
    tls_callbacks = cbs ? (const uint64_t *)cbs : NULL;
}

/* ---- crash reporting ------------------------------------------------------ */
static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
    uint64_t base = (uint64_t)__aw_image_base;
    extern const char __aw_image_size[];
    uint64_t size = (uint64_t)__aw_image_size;
    fprintf(stderr, "[anywin] %s at address %p (rip=%#lx%s, rva=%#lx)\n", strsignal(sig), si->si_addr, rip,
            (rip >= base && rip < base + size) ? " in PE image" : " in runtime",
            (rip >= base && rip < base + size) ? rip - base : 0);
    fprintf(stderr, "[anywin] rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx\n",
            uc->uc_mcontext.gregs[REG_RAX], uc->uc_mcontext.gregs[REG_RBX], uc->uc_mcontext.gregs[REG_RCX],
            uc->uc_mcontext.gregs[REG_RDX], uc->uc_mcontext.gregs[REG_RSI], uc->uc_mcontext.gregs[REG_RDI],
            uc->uc_mcontext.gregs[REG_RSP], uc->uc_mcontext.gregs[REG_RBP]);
    signal(sig, SIG_DFL);
    raise(sig);
}

extern void aw_kernel32_init(int argc, char **argv, char **envp);
extern void aw_seh_init(void);
extern void aw_msvcrt_init(void);
extern int aw_seh_signal(int sig, siginfo_t *si, void *uc);

static void fault_handler(int sig, siginfo_t *si, void *uc)
{
    if (aw_seh_signal(sig, si, uc))
        return;
    crash_handler(sig, si, uc);
}

int main(int argc, char **argv, char **envp)
{
    const char *d = getenv("ANYWIN_DEBUG");
    aw_debug = d && *d && *d != '0';
    d = getenv("ANYWIN_STRICT");
    strict_mode = d && *d && *d != '0';

    pthread_attr_t attr;
    void *stackaddr = NULL;
    size_t stacksize = 8 << 20;
    if (!pthread_getattr_np(pthread_self(), &attr)) {
        pthread_attr_getstack(&attr, &stackaddr, &stacksize);
        pthread_attr_destroy(&attr);
    }
    /* PEB */
    *(void **)(peb + 0x10) = __aw_image_base;
    *(void **)(peb + 0x20) = proc_params;
    *(uint32_t *)(peb + 0x118) = 10;    /* OSMajorVersion */
    *(uint32_t *)(peb + 0x11c) = 0;     /* OSMinorVersion */
    *(uint16_t *)(peb + 0x120) = 19045; /* OSBuildNumber */
    *(uint32_t *)(peb + 0x124) = 2;     /* VER_PLATFORM_WIN32_NT */

    init_tls_directory();
    aw_teb_create((uint8_t *)stackaddr + stacksize, stacksize);
    aw_kernel32_init(argc, argv, envp);
    aw_msvcrt_init();
    aw_seh_init();

    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    run_tls_callbacks(1 /* DLL_PROCESS_ATTACH */);
    TRACE("calling entry point %#lx", (unsigned long)__aw_image_entry);
    int ret = ((int(AWCALL *)(void *))(uintptr_t)__aw_image_entry)(peb);
    extern void WINAPI __aw_kernel32_ExitProcess(UINT);
    __aw_kernel32_ExitProcess(ret);
    return ret;
}
