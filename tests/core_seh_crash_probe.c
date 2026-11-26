/* S8 SEH probe: an unhandled access violation. Built with MinGW gcc.
 * Usage: core_seh_crash_probe.exe [filter]
 *   without an argument the fault is unhandled: stdout has "before" and the process ends in the
 *   runtime's crash report (stderr: exception code, RIP and RVA), killed by SIGSEGV;
 *   with "filter" an UnhandledExceptionFilter runs first and prints its line, then the same. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* the address is 0 but only known at run time (argc - 1), so the compiler keeps the store */
static volatile intptr_t addr_word;

static void __attribute__((noinline)) store_null(void) { *(volatile int *)addr_word = 1; }

static LONG WINAPI report_filter(EXCEPTION_POINTERS *p)
{
    printf("filter code=%lx\n", (unsigned long)p->ExceptionRecord->ExceptionCode);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    addr_word = argc - 1;
    if (argc > 1 && !strcmp(argv[1], "filter"))
        SetUnhandledExceptionFilter(report_filter);
    printf("before\n");
    fflush(stdout);
    store_null();
    printf("not reached\n");
    return 0;
}
