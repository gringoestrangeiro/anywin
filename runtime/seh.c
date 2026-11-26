/* anywin runtime - structured exception handling (S8).
 *
 * Windows x64 model over the PE image's own unwind tables (.pdata/.xdata):
 *   - RtlLookupFunctionEntry / RtlVirtualUnwind (UNWIND_CODE v1, handler flags, no chained info)
 *   - __C_specific_handler (C scope tables: __try/__except filters and __finally funclets)
 *   - RaiseException, UnhandledExceptionFilter (the filter itself is stored by kernel32)
 *   - host signals (SIGSEGV/SIGBUS/SIGFPE/SIGILL) converted to EXCEPTION_RECORD + CONTEXT;
 *     a handled exception resumes by rewriting the ucontext, an unhandled one returns 0 so
 *     start.c prints its RIP/RVA crash report.
 * Frames outside the PE image (runtime C code) carry no unwind data: the walk stops there.
 */
#include "aw.h"
#include "k32.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

extern int aw_msvcrt_fault_signal(int ws);
extern void WINAPI __aw_kernel32_ExitProcess(UINT);
extern const char __aw_image_size[];

/* ---- Windows structures (x64 layout) ---------------------------------- */
#define EXC_CONTINUE_EXECUTION (-1)
#define EXC_CONTINUE_SEARCH 0
#define EXC_EXECUTE_HANDLER 1
#define EXC_NONCONTINUABLE 0x1
#define EXC_UNWINDING 0x2
#define CTX_FULL 0x10000bu /* CONTEXT_AMD64 | CONTROL | INTEGER | FLOATING_POINT */
#define UNW_FLAG_EHANDLER 0x1
#define UNW_FLAG_UHANDLER 0x2
#define UNW_FLAG_CHAININFO 0x4
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_STACK_OVERFLOW 0xC00000FDu
#define STATUS_ILLEGAL_INSTRUCTION 0xC000001Du

typedef struct {
    uint32_t BeginAddress, EndAddress, UnwindData;
} RUNTIME_FUNCTION;

typedef struct {
    uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    uint32_t ContextFlags, MxCsr;
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    uint32_t EFlags;
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    /* Rax..R15 are consecutive and follow the x64 register numbering (see reg_of) */
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint8_t FltSave[512]; /* FXSAVE layout: XMM0 at +0xa0 (CONTEXT +0x1a0) */
    uint8_t VectorRegister[26][16];
    uint64_t VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip,
        LastExceptionFromRip;
} __attribute__((aligned(16))) CONTEXT;

typedef struct {
    uint32_t ExceptionCode, ExceptionFlags;
    void *ExceptionRecord;
    void *ExceptionAddress;
    uint32_t NumberParameters, pad;
    uint64_t ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

typedef struct {
    uint64_t ControlPc, ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    uint64_t EstablisherFrame, TargetIp;
    CONTEXT *ContextRecord;
    void *LanguageHandler, *HandlerData, *HistoryTable;
    uint32_t ScopeIndex, Fill0;
    uint32_t ControlPcIsUnwind;
    uint32_t TargetFrameSet; /* set by __C_specific_handler when a filter picks __except */
    uint64_t TargetFrame;
} DISPATCHER_CONTEXT;

typedef struct {
    uint32_t BeginAddress, EndAddress, HandlerAddress, JumpTarget;
} SCOPE_ENTRY;

typedef int(WINAPI *lang_handler_fn)(EXCEPTION_RECORD *, uint64_t, CONTEXT *, DISPATCHER_CONTEXT *);
typedef void(WINAPI *finally_fn)(uint64_t abnormal, uint64_t frame);
typedef int32_t(WINAPI *filter_fn)(EXCEPTION_POINTERS *, uint64_t frame);

/* ---- PE image access ---------------------------------------------------- */
static uint64_t image_lo(void) { return (uint64_t)__aw_image_base; }
static uint64_t image_hi(void) { return (uint64_t)__aw_image_base + (uint64_t)__aw_image_size; }
static int in_image(uint64_t a) { return a >= image_lo() && a < image_hi(); }

static RUNTIME_FUNCTION *pdata;
static size_t pdata_n;

/* the exception directory (data directory 3) of the image headers, as start.c reads TLS */
static void pdata_init(void)
{
    const uint8_t *base = (const uint8_t *)__aw_image_base;
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    const uint8_t *opt = base + pe + 24;
    uint32_t ndirs = *(const uint32_t *)(opt + 108);
    if (ndirs <= 3)
        return;
    uint32_t rva = *(const uint32_t *)(opt + 112 + 3 * 8);
    uint32_t size = *(const uint32_t *)(opt + 112 + 3 * 8 + 4);
    if (!rva || !size)
        return;
    pdata = (RUNTIME_FUNCTION *)(base + rva);
    pdata_n = size / sizeof *pdata;
}

/* binary search over the sorted .pdata for the function containing an image RVA */
static RUNTIME_FUNCTION *lookup_rva(uint32_t rva)
{
    size_t lo = 0, hi = pdata_n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (pdata[mid].BeginAddress <= rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return NULL;
    RUNTIME_FUNCTION *rf = &pdata[lo - 1];
    return rva < rf->EndAddress ? rf : NULL;
}

/* ---- unwinding ------------------------------------------------------------ */
/* register number (x64 encoding) -> CONTEXT slot */
static uint64_t *reg_of(CONTEXT *c, unsigned n) { return &c->Rax + n; }

static uint64_t *xmm_of(CONTEXT *c, unsigned n) { return (uint64_t *)(c->FltSave + 0xa0 + 16 * n); }

/* one UNWIND_CODE, decoded: a code occupies 1..3 slots of 16 bits */
typedef struct {
    uint32_t offset; /* CodeOffset: instruction end offset inside the prolog */
    uint32_t op, info;
    uint32_t val;    /* stack adjustment or memory offset, depending on op */
    uint32_t slots;
} UCODE;

static int decode_code(const uint16_t *codes, uint32_t count, uint32_t i, UCODE *u)
{
    uint16_t s = codes[i];
    u->offset = s & 0xff;
    u->op = (s >> 8) & 0xf;
    u->info = (s >> 12) & 0xf;
    u->val = 0;
    u->slots = 1;
    if (i >= count)
        return 0;
    switch (u->op) {
    case 0: /* PUSH_NONVOL */
    case 3: /* SET_FPREG */
        break;
    case 1: /* ALLOC_LARGE */
        if (u->info == 0) {
            if (i + 1 >= count) return 0;
            u->val = (uint32_t)codes[i + 1] * 8;
            u->slots = 2;
        } else {
            if (i + 2 >= count) return 0;
            u->val = codes[i + 1] | ((uint32_t)codes[i + 2] << 16);
            u->slots = 3;
        }
        break;
    case 2: /* ALLOC_SMALL */
        u->val = u->info * 8 + 8;
        break;
    case 4: /* SAVE_NONVOL */
    case 8: /* SAVE_XMM128 */
        if (i + 1 >= count) return 0;
        u->val = (uint32_t)codes[i + 1] * (u->op == 4 ? 8 : 16);
        u->slots = 2;
        break;
    case 5: /* SAVE_NONVOL_FAR */
    case 9: /* SAVE_XMM128_FAR */
        if (i + 2 >= count) return 0;
        u->val = codes[i + 1] | ((uint32_t)codes[i + 2] << 16);
        u->slots = 3;
        break;
    default: /* EPILOG, SPARE, PUSH_MACHFRAME: not produced by the toolchains we load */
        return 0;
    }
    return 1;
}

/* Undoes the prolog of the frame described by *c (its Rip is inside the function rf) and
 * leaves the caller's state in *c. est receives the establisher frame. */
static int unwind_frame(uint64_t base, uint64_t pc, const RUNTIME_FUNCTION *rf, CONTEXT *c, uint64_t *est,
                        uint64_t *handler, void **hdata, uint32_t *flags)
{
    const uint8_t *u = (const uint8_t *)(base + rf->UnwindData);
    uint32_t version = u[0] & 7, fl = u[0] >> 3;
    if ((version != 1 && version != 2) || (fl & UNW_FLAG_CHAININFO))
        return 0;
    uint32_t prolog = u[1], count = u[2];
    uint32_t frreg = u[3] & 0xf, froff = u[3] >> 4;
    const uint16_t *codes = (const uint16_t *)(u + 4);
    uint32_t off = (uint32_t)(pc - (base + rf->BeginAddress));
    int in_prolog = off < prolog;

    uint64_t F; /* RSP at the end of the prolog */
    if (frreg) {
        F = *reg_of(c, frreg) - (uint64_t)froff * 16;
    } else if (in_prolog) {
        uint64_t unexecuted = 0;
        for (uint32_t i = 0; i < count;) {
            UCODE k;
            if (!decode_code(codes, count, i, &k))
                return 0;
            if (k.offset > off && (k.op == 0 || k.op == 1 || k.op == 2))
                unexecuted += k.op == 0 ? 8 : k.val;
            i += k.slots;
        }
        F = c->Rsp - unexecuted;
    } else {
        F = c->Rsp;
    }

    /* the executed codes undo the prolog from the innermost outwards; sp walks up the stack */
    uint64_t sp = (frreg && !in_prolog) ? F : c->Rsp;
    for (uint32_t i = 0; i < count;) {
        UCODE k;
        if (!decode_code(codes, count, i, &k))
            return 0;
        i += k.slots;
        if (in_prolog && k.offset > off)
            continue; /* not executed yet: the registers still hold the caller's values */
        switch (k.op) {
        case 0: /* PUSH_NONVOL */
            *reg_of(c, k.info) = *(const uint64_t *)sp;
            sp += 8;
            break;
        case 1: /* ALLOC_LARGE */
        case 2: /* ALLOC_SMALL */
            sp += k.val;
            break;
        case 3: /* SET_FPREG: rsp itself is unchanged */
            break;
        case 4: /* SAVE_NONVOL */
        case 5: /* SAVE_NONVOL_FAR */
            *reg_of(c, k.info) = *(const uint64_t *)(F + k.val);
            break;
        case 8: /* SAVE_XMM128 */
        case 9: /* SAVE_XMM128_FAR */
            memcpy(xmm_of(c, k.info), (const void *)(F + k.val), 16);
            break;
        }
    }
    c->Rip = *(const uint64_t *)sp;
    c->Rsp = sp + 8;
    *est = F;
    *handler = 0;
    *hdata = NULL;
    *flags = fl;
    if (fl & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) {
        uint32_t slots = count + (count & 1);
        *handler = base + *(const uint32_t *)(codes + slots);
        *hdata = (void *)(codes + slots + 2);
    }
    return 1;
}

/* One step of the walk: the frame that *cur is in, and its caller in f->caller.
 * call_site: cur.Rip is a return address, so the call instruction (Rip-1) selects the scope. */
typedef struct {
    uint64_t pc, est, handler;
    void *hdata;
    uint32_t flags;
    RUNTIME_FUNCTION *rf;
    CONTEXT caller;
} FRAME;

static int seh_step(const CONTEXT *cur, int call_site, FRAME *f)
{
    uint64_t base = image_lo();
    f->pc = cur->Rip;
    uint64_t lpc = call_site ? f->pc - 1 : f->pc;
    if (!pdata_n || !in_image(lpc)) /* no unwind data in the image: nothing to walk */
        return 0;
    f->caller = *cur;
    f->rf = lookup_rva((uint32_t)(lpc - base));
    if (!f->rf) { /* leaf without unwind data: the return address is on top of the stack */
        f->est = cur->Rsp;
        f->handler = 0;
        f->hdata = NULL;
        f->flags = 0;
        f->caller.Rip = *(const uint64_t *)cur->Rsp;
        f->caller.Rsp = cur->Rsp + 8;
        return 1;
    }
    return unwind_frame(base, lpc, f->rf, &f->caller, &f->est, &f->handler, &f->hdata, &f->flags);
}

static void call_language_handler(FRAME *f, EXCEPTION_RECORD *rec, CONTEXT *exc, int site,
                                  uint64_t target_ip, uint64_t target_frame, uint32_t scope)
{
    DISPATCHER_CONTEXT dc;
    memset(&dc, 0, sizeof dc);
    dc.ControlPc = f->pc;
    dc.ImageBase = image_lo();
    dc.FunctionEntry = f->rf;
    dc.EstablisherFrame = f->est;
    dc.TargetIp = target_ip;
    dc.TargetFrame = target_frame;
    dc.ScopeIndex = scope;
    dc.ContextRecord = exc;
    dc.LanguageHandler = (void *)f->handler;
    dc.HandlerData = f->hdata;
    dc.ControlPcIsUnwind = site;
    ((lang_handler_fn)f->handler)(rec, f->est, exc, &dc);
}

/* ---- __C_specific_handler ------------------------------------------------ */
/* Scope table: Count, then SCOPE_ENTRY[Count] (RVAs). HandlerAddress 1 = EXECUTE_HANDLER and
 * 0 = CONTINUE_SEARCH constants; any other value is a filter funclet. JumpTarget 0 marks a
 * __finally whose HandlerAddress is the termination funclet. */
API(msvcrt, int, __C_specific_handler, (EXCEPTION_RECORD * rec, uint64_t frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc))
{
    const uint32_t *hd = dc->HandlerData;
    uint32_t count = hd[0];
    const SCOPE_ENTRY *tab = (const SCOPE_ENTRY *)(hd + 1);
    uint64_t base = dc->ImageBase;
    uint32_t pc = (uint32_t)(dc->ControlPc - base) - (dc->ControlPcIsUnwind ? 1u : 0u);
    int unwinding = (rec->ExceptionFlags & EXC_UNWINDING) != 0;

    for (uint32_t i = 0; i < count; i++) {
        const SCOPE_ENTRY *e = &tab[i];
        if (pc < e->BeginAddress || pc >= e->EndAddress)
            continue;
        if (!e->JumpTarget) { /* __finally: runs while unwinding, not inside the target scope */
            if (!unwinding)
                continue;
            if (dc->TargetIp && i >= dc->ScopeIndex)
                break;
            ((finally_fn)(base + e->HandlerAddress))(1, frame);
            continue;
        }
        if (unwinding)
            continue;
        int32_t r;
        if (e->HandlerAddress == 1)
            r = EXC_EXECUTE_HANDLER;
        else if (e->HandlerAddress == 0)
            r = EXC_CONTINUE_SEARCH;
        else {
            EXCEPTION_POINTERS ep = {rec, ctx};
            r = ((filter_fn)(base + e->HandlerAddress))(&ep, frame);
        }
        if (r == EXC_CONTINUE_SEARCH)
            continue;
        if (r == EXC_CONTINUE_EXECUTION)
            return 0; /* ExceptionContinueExecution */
        /* EXECUTE_HANDLER: the dispatcher unwinds to this scope (see unwind_to) */
        dc->ScopeIndex = i;
        dc->TargetIp = base + e->JumpTarget;
        dc->TargetFrame = frame;
        dc->TargetFrameSet = 1;
        return 1;
    }
    return 1; /* ExceptionContinueSearch */
}

/* ---- dispatch ---------------------------------------------------------- */
/* Second pass: runs the termination handlers of the frames below the target, then the target
 * frame's own handler (finally blocks inside it), and leaves *resume at the __except body. */
static int unwind_to(EXCEPTION_RECORD *rec, CONTEXT *exc, const DISPATCHER_CONTEXT *target, int call_site,
                     CONTEXT *resume)
{
    CONTEXT cur = *exc;
    uint32_t saved = rec->ExceptionFlags;
    rec->ExceptionFlags |= EXC_UNWINDING;
    for (int depth = 0; depth < 512; depth++) {
        FRAME f;
        int site = depth > 0 || call_site;
        if (!seh_step(&cur, site, &f))
            break;
        if (f.est == target->TargetFrame) {
            if (f.handler && (f.flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)))
                call_language_handler(&f, rec, exc, site, target->TargetIp, target->TargetFrame,
                                      target->ScopeIndex);
            *resume = cur;
            resume->Rip = target->TargetIp;
            resume->Rax = rec->ExceptionCode; /* __except bodies read GetExceptionCode() from RAX */
            rec->ExceptionFlags = saved;
            return 1;
        }
        if (f.handler && (f.flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)))
            call_language_handler(&f, rec, exc, site, 0, 0, 0);
        cur = f.caller;
    }
    rec->ExceptionFlags = saved;
    return 0;
}

/* First pass: language handlers from the faulting frame outwards. Returns 1 when *resume holds
 * the context to continue at (ExceptionContinueExecution or a unwind to an __except body). */
static int dispatch(EXCEPTION_RECORD *rec, CONTEXT *exc, int call_site, CONTEXT *resume)
{
    CONTEXT cur = *exc;
    for (int depth = 0; depth < 512; depth++) {
        FRAME f;
        int site = depth > 0 || call_site;
        if (!seh_step(&cur, site, &f))
            break;
        if (f.handler && (f.flags & UNW_FLAG_EHANDLER)) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof dc);
            dc.ControlPc = f.pc;
            dc.ImageBase = image_lo();
            dc.FunctionEntry = f.rf;
            dc.EstablisherFrame = f.est;
            dc.ContextRecord = exc;
            dc.LanguageHandler = (void *)f.handler;
            dc.HandlerData = f.hdata;
            dc.ControlPcIsUnwind = site;
            int d = ((lang_handler_fn)f.handler)(rec, f.est, exc, &dc);
            if (dc.TargetFrameSet)
                return unwind_to(rec, exc, &dc, call_site, resume);
            if (d == 0) { /* ExceptionContinueExecution: the filter may have edited *exc */
                *resume = *exc;
                return 1;
            }
        }
        cur = f.caller;
    }
    return 0;
}

/* UnhandledExceptionFilter of the program (SetUnhandledExceptionFilter) gets the last word. */
static int unhandled(EXCEPTION_RECORD *rec, CONTEXT *exc, CONTEXT *resume)
{
    void *flt = aw_unhandled_filter();
    if (!flt)
        return 0;
    EXCEPTION_POINTERS ep = {rec, exc};
    int32_t r = ((filter_fn)flt)(&ep, 0);
    if (r != EXC_CONTINUE_EXECUTION)
        return 0;
    *resume = *exc;
    return 1;
}

API(kernel32, int32_t, UnhandledExceptionFilter, (EXCEPTION_POINTERS * ep))
{
    (void)ep;
    return EXC_EXECUTE_HANDLER; /* default action: the process terminates */
}

API(ntdll, RUNTIME_FUNCTION *, RtlLookupFunctionEntry, (uint64_t pc, uint64_t *imagebase, void *history))
{
    (void)history;
    if (!in_image(pc))
        return NULL;
    if (imagebase)
        *imagebase = image_lo();
    return lookup_rva((uint32_t)(pc - image_lo()));
}

API(ntdll, void *, RtlVirtualUnwind, (uint32_t type, uint64_t imagebase, uint64_t pc, RUNTIME_FUNCTION *rf,
                                     CONTEXT *ctx, void **hdata, uint64_t *est, void *ctxptrs))
{
    (void)type;
    (void)ctxptrs;
    uint64_t handler = 0;
    uint32_t fl = 0;
    void *hd = NULL;
    uint64_t e = 0;
    if (!rf || !unwind_frame(imagebase, pc, rf, ctx, &e, &handler, &hd, &fl))
        return NULL;
    if (hdata)
        *hdata = hd;
    if (est)
        *est = e;
    return (void *)handler;
}

/* ---- RaiseException -------------------------------------------------------- */
/* Entry stub: captures the caller's state (Rip = return address, Rsp = after it, callee-saved
 * registers untouched) into a CONTEXT and calls aw_seh_raise_c(ctx, args). */
__asm__(".text\n"
        ".globl __aw_kernel32_RaiseException\n"
        "__aw_kernel32_RaiseException:\n"
        "\tpush %rbp\n"
        "\tmov %rsp, %rbp\n"
        "\tsub $0x4f0, %rsp\n"
        "\tmov %rax, 0x78(%rsp)\n"
        "\tmovl $0x10000b, 0x30(%rsp)\n"
        "\tmov %rcx, 0x80(%rsp)\n"
        "\tmov %rdx, 0x88(%rsp)\n"
        "\tmov %rbx, 0x90(%rsp)\n"
        "\tlea 0x10(%rbp), %rax\n"
        "\tmov %rax, 0x98(%rsp)\n"
        "\tmov (%rbp), %rax\n"
        "\tmov %rax, 0xa0(%rsp)\n"
        "\tmov %rsi, 0xa8(%rsp)\n"
        "\tmov %rdi, 0xb0(%rsp)\n"
        "\tmov %r8, 0xb8(%rsp)\n"
        "\tmov %r9, 0xc0(%rsp)\n"
        "\tmov %r10, 0xc8(%rsp)\n"
        "\tmov %r11, 0xd0(%rsp)\n"
        "\tmov %r12, 0xd8(%rsp)\n"
        "\tmov %r13, 0xe0(%rsp)\n"
        "\tmov %r14, 0xe8(%rsp)\n"
        "\tmov %r15, 0xf0(%rsp)\n"
        "\tmov 8(%rbp), %rax\n"
        "\tmov %rax, 0xf8(%rsp)\n"
        "\tfxsave 0x100(%rsp)\n"
        "\tmov %rcx, 0x4d0(%rsp)\n"
        "\tmov %rdx, 0x4d8(%rsp)\n"
        "\tmov %r8, 0x4e0(%rsp)\n"
        "\tmov %r9, 0x4e8(%rsp)\n"
        "\tmov %rsp, %rdi\n"
        "\tlea 0x4d0(%rsp), %rsi\n"
        "\tcall aw_seh_raise_c\n"
        "\thlt\n");

/* Resumes the context: loads every register it saved and jumps to Rip (RtlRestoreContext). */
__asm__(".text\n"
        ".globl aw_restore_context\n"
        "aw_restore_context:\n"
        "\tmov %rdi, %r11\n"
        "\tfxrstor 0x100(%r11)\n"
        "\tmov 0x90(%r11), %rbx\n"
        "\tmov 0xa0(%r11), %rbp\n"
        "\tmov 0xa8(%r11), %rsi\n"
        "\tmov 0xb0(%r11), %rdi\n"
        "\tmov 0xb8(%r11), %r8\n"
        "\tmov 0xc0(%r11), %r9\n"
        "\tmov 0xc8(%r11), %r10\n"
        "\tmov 0xd8(%r11), %r12\n"
        "\tmov 0xe0(%r11), %r13\n"
        "\tmov 0xe8(%r11), %r14\n"
        "\tmov 0xf0(%r11), %r15\n"
        "\tmov 0x80(%r11), %rcx\n"
        "\tmov 0x88(%r11), %rdx\n"
        "\tmov 0x78(%r11), %rax\n"
        "\tmov 0x98(%r11), %rsp\n"
        "\tjmp *0xf8(%r11)\n");

extern void aw_restore_context(const CONTEXT *c) __attribute__((noreturn));
void aw_seh_raise_c(CONTEXT *ctx, const uint64_t *a) __attribute__((used, noreturn));

void aw_seh_raise_c(CONTEXT *ctx, const uint64_t *a)
{
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = (uint32_t)a[0];
    rec.ExceptionFlags = (uint32_t)a[1] & EXC_NONCONTINUABLE;
    rec.ExceptionAddress = (void *)ctx->Rip;
    uint32_t n = (uint32_t)a[2];
    if (n > 15)
        n = 15;
    if (n && a[3])
        for (uint32_t i = 0; i < n; i++)
            rec.ExceptionInformation[i] = ((const uint64_t *)a[3])[i];
    rec.NumberParameters = n;
    CONTEXT resume;
    if (dispatch(&rec, ctx, 1, &resume) || unhandled(&rec, ctx, &resume))
        aw_restore_context(&resume);
    fprintf(stderr, "[anywin] unhandled exception %#x at %p\n", rec.ExceptionCode, rec.ExceptionAddress);
    fflush(stderr);
    __aw_kernel32_ExitProcess((UINT)rec.ExceptionCode);
    _exit(3);
}

void WINAPI __aw_kernel32_RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);
AW_EXPORT_ENTRY(kernel32, "RaiseException", __aw_kernel32_RaiseException)

/* ---- host signals ---------------------------------------------------------- */
static __thread int seh_busy;
/* a handler that resumes at the faulting instruction without changing it faults again at once;
 * after many such rounds the fault is reported instead of looping forever */
static __thread uint64_t inplace_rip;
static __thread int inplace_count;

static void ctx_from_uc(const ucontext_t *uc, CONTEXT *c)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    memset(c, 0, sizeof *c);
    c->ContextFlags = CTX_FULL;
    c->Rax = g[REG_RAX];
    c->Rcx = g[REG_RCX];
    c->Rdx = g[REG_RDX];
    c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP];
    c->Rbp = g[REG_RBP];
    c->Rsi = g[REG_RSI];
    c->Rdi = g[REG_RDI];
    c->R8 = g[REG_R8];
    c->R9 = g[REG_R9];
    c->R10 = g[REG_R10];
    c->R11 = g[REG_R11];
    c->R12 = g[REG_R12];
    c->R13 = g[REG_R13];
    c->R14 = g[REG_R14];
    c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP];
    c->EFlags = (uint32_t)g[REG_EFL];
    if (uc->uc_mcontext.fpregs) {
        memcpy(c->FltSave, uc->uc_mcontext.fpregs, sizeof c->FltSave);
        c->MxCsr = *(const uint32_t *)(c->FltSave + 24);
    }
}

static void ctx_to_uc(const CONTEXT *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax;
    g[REG_RCX] = c->Rcx;
    g[REG_RDX] = c->Rdx;
    g[REG_RBX] = c->Rbx;
    g[REG_RSP] = c->Rsp;
    g[REG_RBP] = c->Rbp;
    g[REG_RSI] = c->Rsi;
    g[REG_RDI] = c->Rdi;
    g[REG_R8] = c->R8;
    g[REG_R9] = c->R9;
    g[REG_R10] = c->R10;
    g[REG_R11] = c->R11;
    g[REG_R12] = c->R12;
    g[REG_R13] = c->R13;
    g[REG_R14] = c->R14;
    g[REG_R15] = c->R15;
    g[REG_RIP] = c->Rip;
    g[REG_EFL] = c->EFlags;
    if (uc->uc_mcontext.fpregs)
        memcpy(uc->uc_mcontext.fpregs, c->FltSave, sizeof c->FltSave);
}

/* Converts a host fault into a Windows exception. Returns 1 when a handler resumed execution
 * (the ucontext is rewritten and the kernel restores it), 0 when the fault is unhandled: the
 * caller (start.c) then prints the RIP/RVA report and terminates with the signal. */
void aw_crash_report(int sig, siginfo_t *si, void *uc_);

int aw_seh_signal(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    if (seh_busy)
        return 0; /* a fault while dispatching a fault: report it and stop */
    uint32_t code;
    int winsig;
    uint64_t addr = (uint64_t)si->si_addr;
    uint64_t rsp = uc->uc_mcontext.gregs[REG_RSP];
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        code = STATUS_ACCESS_VIOLATION;
        /* a fault just below the stack pointer is the guard page: report it as overflow */
        if (addr < rsp + 0x1000 && addr + 0x10000 >= rsp)
            code = STATUS_STACK_OVERFLOW;
        winsig = 11; /* SIGSEGV: the C signal() number the program knows */
        break;
    case SIGFPE:
        winsig = 8;
        switch (si->si_code) {
        case FPE_INTDIV: code = 0xC0000094u; break;
        case FPE_INTOVF: code = 0xC0000095u; break;
        case FPE_FLTDIV: code = 0xC000008Eu; break;
        case FPE_FLTOVF: code = 0xC0000091u; break;
        case FPE_FLTUND: code = 0xC0000093u; break;
        case FPE_FLTRES: code = 0xC000008Fu; break;
        case FPE_FLTINV: code = 0xC0000090u; break;
        case FPE_FLTSUB: code = 0xC000008Du; break;
        default: code = 0xC0000094u; break;
        }
        break;
    case SIGILL:
        code = STATUS_ILLEGAL_INSTRUCTION;
        winsig = 4;
        break;
    default:
        return 0;
    }

    uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
    if (rip == inplace_rip) {
        if (++inplace_count > 64)
            return 0;
    } else {
        inplace_rip = 0;
        inplace_count = 0;
    }

    CONTEXT ctx, resume;
    ctx_from_uc(uc, &ctx);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code;
    rec.ExceptionAddress = (void *)rip;
    if (code == STATUS_ACCESS_VIOLATION || code == STATUS_STACK_OVERFLOW) {
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (uc->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0;
        rec.ExceptionInformation[1] = addr;
    }

    seh_busy = 1;
    int resumed = dispatch(&rec, &ctx, 0, &resume);
    if (!resumed)
        resumed = unhandled(&rec, &ctx, &resume);
    seh_busy = 0;
    if (resumed) {
        ctx_to_uc(&resume, uc);
        inplace_rip = resume.Rip == rip ? rip : 0;
        return 1;
    }
    /* the C runtime's signal() handler (if the program installed one) runs before the report */
    (void)aw_msvcrt_fault_signal(winsig);
    fprintf(stderr, "[anywin] unhandled exception %#x\n", code);
    aw_crash_report(sig, si, uc_);
    /* the process ends with the exception code as its exit status, as on Windows */
    __aw_kernel32_ExitProcess(code);
    return 0;
}

static void seh_fault(int sig, siginfo_t *si, void *uc)
{
    if (aw_seh_signal(sig, si, uc))
        return;
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Alternate signal stack and the fault handlers. start.c installs its own handlers after this
 * call (see the report); both route to aw_seh_signal. */
void aw_seh_init(void)
{
    pdata_init();
    size_t sz = 256 * 1024;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = malloc(sz);
    ss.ss_size = ss.ss_sp ? sz : 0;
    if (ss.ss_sp)
        sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = seh_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}


/* kernel32 names for the unwind helpers Rust and MinGW code import from KERNEL32 */
API(kernel32, int, __C_specific_handler, (EXCEPTION_RECORD * rec, uint64_t frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc))
{
    return __aw_msvcrt___C_specific_handler(rec, frame, ctx, dc);
}

API(kernel32, RUNTIME_FUNCTION *, RtlLookupFunctionEntry, (uint64_t pc, uint64_t *imagebase, void *history))
{
    return __aw_ntdll_RtlLookupFunctionEntry(pc, imagebase, history);
}

API(kernel32, void *, RtlVirtualUnwind, (uint32_t type, uint64_t imagebase, uint64_t pc, RUNTIME_FUNCTION *rf,
                                         CONTEXT *ctx, void **hdata, uint64_t *est, void *ctxptrs))
{
    return __aw_ntdll_RtlVirtualUnwind(type, imagebase, pc, rf, ctx, hdata, est, ctxptrs);
}
