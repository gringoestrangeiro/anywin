/* msvcrt core probe for tests/core_stdio.sh (S2). Built with the default mingw-w64 toolchain
 * (UCRT import names, see PLAN-CORE 3.2 and runtime/msvcrt.c). argv[1] is the scratch
 * directory as a Windows path; the probe writes a file there with backslash paths.
 * stdout is compared line by line by the script; stderr is compared separately. */
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void done_a(void) { printf("atexit A\n"); }
static void done_b(void) { printf("atexit B\n"); }
static void done_c(void) { printf("atexit C\n"); }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "Z:\\tmp";
    char path[512], missing[512], small[8], buf[64];

    /* integer conversions */
    printf("d=%d u=%u x=%x X=%X o=%o\n", -42, 42u, 255, 255, 8);
    printf("ld=%ld lu=%lu lx=%lx\n", -7L, 4000000000UL, 0xdeadbeefUL);
    printf("lld=%lld llu=%llu\n", -1234567890123LL, 18446744073709551615ULL);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    printf("I64d=%I64d I64x=%I64x\n", 1234567890123LL, 255LL); /* I64 is not checked by gcc */
#pragma GCC diagnostic pop
    printf("h=[%hd] [%hhu]\n", 70000, 300);
    printf("sizes=[%zu] [%td]\n", (size_t)5, (ptrdiff_t)-3);

    /* width, precision, flags, star */
    printf("width [%5d] [%-5d] [%05d] [%+d] [% d]\n", 42, 42, 42, 42, 42);
    printf("prec [%.3d] [%8.3d] [%.0d]\n", 7, 7, 0);
    printf("star [%*d] [%-*d|] [%.*s]\n", 6, 9, 4, 1, 2, "abcdef");
    printf("alt [%#x] [%#X] [%#o]\n", 255, 255, 8);
    printf("pct [%%] char [%c%c] [%3c] [%-3c|]\n", 'o', 'k', 'z', 'y');

    /* strings and pointers */
    printf("str [%s] [%10s] [%-10s|] [%.3s]\n", "hello", "hi", "hi", "truncate");
    char *volatile nul = NULL;
    printf("null=[%s]\n", nul);
    printf("ptr [%p] [%p]\n", (void *)(uintptr_t)0x1234ABCDu, (void *)NULL);

    /* floats (glibc formatting, msvcrt-compatible exponent width is UCRT two-digit here) */
    printf("float [%f] [%.1f] [%8.2f]\n", 1.5, 2.26, -3.14159);
    printf("exp [%e] [%.2E] [%g] [%G]\n", 1.0, 12345.678, 0.0001, 1e20);
    printf("inf [%f] [%f] [%+f]\n", 1.0 / 0.0, -1.0 / 0.0, 1.0 / 0.0);

    /* sprintf family */
    sprintf(buf, "%d-%s|%5.2f", 7, "x", 2.5);
    printf("sprintf=[%s]\n", buf);
    int n = snprintf(buf, sizeof buf, "%d-%s", 7, "xy");
    printf("snprintf n=%d buf=[%s]\n", n, buf);
    const char *volatile ten = "abcdefghij"; /* volatile: keep gcc from folding the truncation */
    n = snprintf(small, sizeof small, "%s", ten);
    printf("truncated n=%d small=[%s]\n", n, small);

    /* string.h and ctype.h */
    printf("strchr=[%s] strrchr=[%s]\n", strchr("a/b/c", '/'), strrchr("a/b/c", '/'));
    printf("stricmp=%d lower=[%s] upper=%c%c\n", _stricmp("HeLLo", "hello"), "ABC", toupper('q'), tolower('Q'));
    printf("memcmp=%d strerror=[%s]\n", memcmp("abc", "abd", 3) < 0 ? -1 : 0, strerror(ENOENT));
    printf("decimal_point=[%s]\n", localeconv()->decimal_point);

    /* wide strings (UTF-16 converted to UTF-8), %n */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    printf("wide=[%ls] [%S] [%lc]\n", L"wide", L"w2", L'!');
#pragma GCC diagnostic pop
    int cnt = -1;
    printf("abc%n", &cnt);
    printf(" n=%d\n", cnt);

    /* files: backslash paths, binary modes, fwrite/fread, missing file */
    snprintf(path, sizeof path, "%s\\data.bin", dir);
    snprintf(missing, sizeof missing, "%s\\no-such-file.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("fopen write FAILED\n");
        return 1;
    }
    fprintf(f, "line %d\n", 5);
    fwrite("0123456789", 1, 10, f);
    fputc('Z', f);
    fclose(f);

    f = fopen(path, "rb");
    if (!f) {
        printf("fopen read FAILED\n");
        return 1;
    }
    char rb[64];
    memset(rb, 0, sizeof rb);
    size_t got = fread(rb, 1, sizeof rb - 1, f);
    fclose(f);
    printf("read=%u bytes first=[%.6s] last=[%c]\n", (unsigned)got, rb, rb[got - 1]);
    printf("missing=[%s]\n", fopen(missing, "rb") ? "open" : "null");

    /* time */
    time_t t = 1700000000;
    struct tm *tm = localtime(&t);
    char tb[64];
    size_t k = strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", tm);
    printf("time=[%s] len=%u\n", tb, (unsigned)k);
    k = strftime(tb, sizeof tb, "%A %B %j", tm);
    printf("names=[%s] len=%u\n", tb, (unsigned)k);
    printf("now_ok=%d\n", time(NULL) > 1600000000);

    /* stdio plumbing */
    fputs("fputs line\n", stdout);
    putchar('P');
    putchar('\n');
    puts("puts line");
    fflush(stdout);
    fprintf(stderr, "to stderr %d\n", 2);

    /* exit order: atexit handlers run last-in first-out after main returns */
    atexit(done_a);
    atexit(done_b);
    atexit(done_c);
    printf("main done\n");
    return 0;
}
