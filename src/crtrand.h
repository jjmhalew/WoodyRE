/* crtrand.h - the one random number stream of Woody.exe: the MSVC C runtime rand() it links (0x49a7bd).
 *
 * rand 0x49a7bd: holdrand = holdrand * 214013 + 2531011 in the per-thread data (_getptd 0x49c6d3, +0x14), returns
 * (holdrand >> 16) & 0x7fff. The thread data starts with holdrand = 1 and the exe never calls srand, so the stream starts at 1
 * when the process starts and runs on through every level load, restart, save and load: nothing ever reseeds it. All 161 call
 * sites are on the game thread and draw from it in turn (the wrappers below, the VM's RANDOM 0x443c74, the BlackBox mini game,
 * the sound manager), so a draw anywhere shifts every later one. Woody.exe has no other generator (docs/RANDOM.md).
 *
 * crt_rand() is defined in ekovm.c: that file is linked by both the game and the ekorun test harness. */
#ifndef CRTRAND_H
#define CRTRAND_H
#include <stdint.h>

uint32_t crt_rand(void);                                                         /* 0x49a7bd (and 0x43ff00, a jmp to it): 0..32767 */
void crt_srand(uint32_t seed);                                                   /* tests only: Woody.exe has no srand */
static inline int crt_mod(int n) { return (int)crt_rand() % n; }                 /* 0x43ff10(n): rand() % n */
static inline int crt_range(int a, int b) { return a + (int)crt_rand() % (b - a); }   /* 0x43ff20(a, b): a + rand() % (b - a) */
static inline float crt_rnd(void) { return (float)crt_rand() / 32767.0f; }       /* 0x43ff40: rand() / 32767.0 ([0x4b3398]), so [0, 1] INCLUSIVE */
static inline float crt_rnd_pm(void) { return (0.5f - crt_rnd()) * 2.0f; }       /* 0x43ff60: (0.5 - rand() / 32767.0) * 2, [-1, 1] */

#endif
