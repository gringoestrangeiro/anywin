/* S8 SEH probe: C structured exception handling (__try/__except/__finally) and RaiseException.
 * MinGW gcc has no __try, so this file is built with clang (-fms-extensions, mingw target);
 * tests/core_seh.sh links the object with x86_64-w64-mingw32-gcc.
 * Prints one line per case; exit status is the number of failed cases. */
#include <windows.h>
#include <stdio.h>

static volatile int *nullp;
static int fails;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        fails++;
}

/* the fault happens in a callee, so the walk unwinds one PE frame before reaching the __try */
static void __attribute__((noinline)) store_null(void) { *nullp = 1; }

/* the filter expression calls a function: exercises the filter funclet */
static int is_handled_code(DWORD code) { return code == EXCEPTION_ACCESS_VIOLATION || code == 0xE0000001; }

/* returns 1 when the body ran to its end, 2 when __except took over */
static int guarded(int mode, int *finally_ran)
{
    int r = 0;
    __try {
        __try {
            if (mode == 1)
                store_null();
            if (mode == 2)
                RaiseException(0xE0000001, 0, 0, NULL);
            r = 1;
        } __finally {
            *finally_ran = 1;
        }
    } __except (is_handled_code(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        r = 2;
        if (mode == 2)
            r = GetExceptionCode() == 0xE0000001 ? 3 : 4;
    }
    return r;
}

int main(void)
{
    int fin = 0;
    check("no fault runs the body", guarded(0, &fin) == 1 && fin == 1);
    fin = 0;
    check("access violation caught by __except", guarded(1, &fin) == 2 && fin == 1);
    fin = 0;
    check("RaiseException caught with its code", guarded(2, &fin) == 3 && fin == 1);
    printf("done failures=%d\n", fails);
    return fails;
}
