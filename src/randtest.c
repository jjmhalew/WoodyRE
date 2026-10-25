/* randtest.c - checks crt_rand() (crtrand.h) against the MSVC CRT rand() that Woody.exe links (0x49a7bd), on any host.
 *   python -m ziglang cc -std=c99 -O2 -o out/randtest.exe src/ekovm.c src/randtest.c && ./out/randtest.exe
 *   (Linux: cc -std=c99 -O2 -o randtest src/ekovm.c src/randtest.c && ./randtest)
 * Prints the first draws and "ok" / "FAIL"; the exit code is the number of failures. */
#include "crtrand.h"
#include <stdio.h>

int main(void)
{
    static const uint32_t first[10] = { 41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464 };   /* MSVC rand() after srand(1) */
    int fail = 0;
    printf("first draws:");
    for (int i = 0; i < 10; i++) { uint32_t r = crt_rand(); printf(" %u", r); if (r != first[i]) fail++; }
    printf("\n");
    crt_srand(1); if (crt_rand() != 41) { printf("crt_srand(1) does not restart the stream\n"); fail++; }
    crt_srand(1); uint32_t lo = 0x7fff, hi = 0;                                    /* the range is 0..32767 */
    for (int i = 0; i < 1000000; i++) { uint32_t r = crt_rand(); if (r < lo) lo = r; if (r > hi) hi = r; }
    printf("1e6 draws: min %u max %u\n", lo, hi); if (lo != 0 || hi != 32767) fail++;
    crt_srand(1);                                                                    /* the wrappers draw exactly once each */
    float f = crt_rnd(); int m = crt_mod(100), g = crt_range(10, 20); float pm = crt_rnd_pm(), f5 = crt_rnd();
    printf("0x43ff40 %.6f  0x43ff10(100) %d  0x43ff20(10,20) %d  0x43ff60 %.6f  next 0x43ff40 %.6f\n", f, m, g, pm, f5);
    if (f != 41.0f / 32767.0f || m != 18467 % 100 || g != 10 + 6334 % 10 || pm != (0.5f - 26500.0f / 32767.0f) * 2.0f || f5 != 19169.0f / 32767.0f) fail++;
    printf("%s\n", fail ? "FAIL" : "ok");
    return fail;
}
