/* CPU/CRT micro-benchmark used by tests/bench.py (console program, runs under anywin and Wine). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned long long ms(void) { return GetTickCount64(); }

static void isort(unsigned *a, int lo, int hi) { /* iterative-ish quicksort, no CRT qsort dependency */
    while (lo < hi) {
        unsigned p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { unsigned t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { isort(a, lo, j); lo = i; } else { isort(a, i, hi); hi = j; }
    }
}

int main(void) {
    unsigned long long t0, t1, tot0 = ms();
    volatile uint64_t sink = 0;
    /* 1. integer hashing loop */
    t0 = ms();
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t i = 0; i < 400000000ULL; i++) { h ^= i; h *= 1099511628211ULL; }
    sink = h; t1 = ms();
    printf("int_loop_ms %llu\n", t1 - t0);
    /* 2. malloc/free churn */
    t0 = ms();
    void *ptrs[1024] = {0};
    for (int i = 0; i < 6000000; i++) {
        int k = (i * 2654435761u) & 1023;
        free(ptrs[k]);
        ptrs[k] = malloc(16 + (i & 255));
        if (ptrs[k]) ((char *)ptrs[k])[0] = (char)i;
    }
    for (int k = 0; k < 1024; k++) free(ptrs[k]);
    t1 = ms();
    printf("malloc_ms %llu\n", t1 - t0);
    /* 3. sort 4M unsigned */
    t0 = ms();
    int n = 4000000;
    unsigned *a = malloc((size_t)n * sizeof *a);
    unsigned x = 12345;
    for (int i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; a[i] = x; }
    isort(a, 0, n - 1);
    sink = a[n / 2]; free(a); t1 = ms();
    printf("sort_ms %llu\n", t1 - t0);
    /* 4. snprintf */
    t0 = ms();
    char buf[128]; size_t len = 0;
    for (int i = 0; i < 1500000; i++) len += (size_t)snprintf(buf, sizeof buf, "item %d: %s %5.2f 0x%x", i, "todo", i * 0.5, i);
    sink = len; t1 = ms();
    printf("snprintf_ms %llu\n", t1 - t0);
    /* 5. memcpy 2 GB */
    t0 = ms();
    char *s = malloc(1 << 20), *d = malloc(1 << 20);
    memset(s, 1, 1 << 20);
    for (int i = 0; i < 2048; i++) memcpy(d, s, 1 << 20);
    sink = d[5]; free(s); free(d); t1 = ms();
    printf("memcpy_ms %llu\n", t1 - t0);
    printf("total_ms %llu\n", ms() - tot0);
    (void)sink;
    return 0;
}
