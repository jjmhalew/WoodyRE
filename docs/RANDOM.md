# RANDOM.md — the random numbers of Woody.exe

Woody.exe has exactly one random number generator: the MSVC 6 C runtime `rand()` it links statically. Every random
value of the game — script `RANDOM`, enemy choices, camera shake, particles, the sky flashes, the BlackBox mini game —
is a draw from that one stream, so a draw anywhere shifts every later one. The port reproduces it in `src/crtrand.h`
(`crt_rand()`, defined in `src/ekovm.c`). Found with `tools/drange.py` and a scan of every `call` to the generator and its
wrappers in `out/disasm_full.txt`.

## 1. The generator

```
0x49a7bd  rand():  ptd = _getptd() (0x49c6d3);  ptd+0x14 = ptd+0x14 * 214013 + 2531011;  return (ptd+0x14 >> 16) & 0x7fff
```

* The state lives in the per-thread data and starts at **1** when the thread starts. There is **no `srand`** in the exe
  (nothing else writes `ptd+0x14`), so the stream starts at 1 at process start and is never reseeded: not at a level
  load, a level restart, a death, a save or a load. The first values are 41, 18467, 6334, 26500, 19169, 15724, ...
* All 161 call sites are game-thread code. The threads the exe creates (`0x49b3b8` from `0x48d602`, `0x4931ad`,
  `0x493cee`, `0x493d29`: sound / film library) never call it.
* Wrappers (`0x43ff00`..`0x43ff7f`), with their port names:

| address | does | callers | port |
|---|---|---|---|
| `0x43ff00` | `jmp rand` | 11 | `crt_rand()` |
| `0x43ff10(n)` | `rand() % n` (signed `idiv`) | 13 | `crt_mod(n)` |
| `0x43ff20(a, b)` | `a + rand() % (b - a)` | 2 | `crt_range(a, b)` |
| `0x43ff40` | `rand() / 32767.0f` (`[0x4b3398]`) — **[0, 1] inclusive** | 133 | `crt_rnd()` |
| `0x43ff60` | `(0.5 - rand() / 32767.0) * 2` | 1 | `crt_rnd_pm()` |
| `0x49a7bd` direct | the VM's `RANDOM` `0x443c74`: `rand() % pop` | 1 | `crt_rand()` in `ekovm.c` |

A host `rand()` is not a substitute: glibc's differs from MSVC's and its `RAND_MAX` is 2147483647, not 32767, so
`rand() / RAND_MAX` and `rand() % 6283` gave other numbers and other distributions on Linux / Android / Switch than on
Windows. The port has no host `rand()` / `srand()` / `RAND_MAX` left in `src/`.

## 2. Who draws (by function of Woody.exe)

| function | draws | port |
|---|---|---|
| `0x4367f0` level load (`0x40456e`, in LoadFiles before LoadResources and the script init) | 50 × 6 `0x43ff40` into `[0x54ba28]`, which nothing ever reads | `level_load`: 300 `crt_rand()` |
| `0x443c50` VM `RANDOM` | 1 | `ekovm.c` op 62 |
| `0x41fbd0` camera shake | 3 × `0x43ff10(1000)` (two copies of the code) | `cam_update` |
| `0x41c180` Wander choice, `0x41d2c0` random free sensor, `0x41ba10` random angle | 1, 1 (only if a direction is free), 1 | `enemy.c` / `boss.c` `wander_choose`, `enemy_sensor_random_free` |
| `0x41bdf0` Chase stuck hook | 2 (`& 31`) | `enemy_common_move` |
| `0x41b000` dying enemy stars | 1 (`& 1`, before the pool is searched) | `game_enemy_stars` |
| `0x4139b0` ghost Reset | 1 (`& 1`, the hand), after the behaviour restart | `enemies_add` / `enemy_reset`, type 13 only |
| `0x411c80` bomb thrower idle | `0x43ff10(5)` | `bomber_update` |
| `0x40dd30` Buzz 2 taunts | 2 (two copies) | `boss15_update` |
| `0x457516` air attack cry, `0x464545` idle variation | `0x43ff20(0, 3)`, `0x43ff20(0, 7)` | `player.c` |
| `0x4695f0` animation sounds | 1 per call when `0x43a880` collected ≥ 1 type-4 event | `anim_sounds` (lazy draw on the first hit = the same) |
| `0x451cc0` storm, `0x46d520` bolt, `0x46da00` rod arc, `0x46e030` / `0x46e0d0` sky flash | 4, 5, 5, 3, 2 | `storm.c` |
| `0x46e4a0` / `0x46e530` laser | 2 at init; per frame 1 + (zigzag 2 per point) + 1 flicker + sparks (`0x43ff10(0x55)` + `0x43ff40`, 50 a second, thrown away) + impact size | `laser_fx_init` / `laser_fx_draw` |
| `0x472560`, `0x4727d3`, `0x472b40`, `0x47d440`, `0x47e050`, `0x47e160` ambient (class 90) and title birds | 1, 3, 3 (+ 400 thrown away), 5, 5, 3 | `ambient.c` |
| `0x472f50`, `0x473120`, `0x4738c0` (Draw, `0x4741d6`), `0x4747f0` water | 1, 2, 1, 4 | `water.c` |
| `0x470420`, `0x4750e0`, `0x475440`, `0x475d90`, `0x476140`, `0x4767f0`, `0x476b50`, `0x476cd0`, `0x477980`, `0x478360`, `0x478660`, `0x478b70`, `0x4792d0`, `0x479670`, `0x4798f0`, `0x47a8d0`, `0x47cd00`, `0x47cf10`, `0x47e460` particles | see PARTICLES.md, SPLASH.md, BOMB.md, PROJECTILES.md | `main_engine.c` effects |
| `0x47b800` / `0x47ba10` HUD bonus flight ghosts | 3 / 1 | `hud.c` `fly_trail` |
| `0x4866d0`, `0x486a09`, `0x4880b0` BlackBox Woody / landing / Knothead and Splinter | `0x43ff10(4)` / `(3)`, `(3)`, `(4)` | `blackbox.c` |
| `0x468b40(25, 4)` BlackBox footstep chain shuffle, every walking frame | 4 × 2 | `steps_shuffle` (the swapped `next` links are kept for the whole process, as the static sound table) |
| `0x4698f0` sound LFO waveform 4 (noise, `0x43ff60`) | 1 per LFO evaluation | **not ported** (messages 1653 / 1654; no shipped script sends them) |
| `0x41c940` patrol: idle or turn at a path point | `0x43ff10(60)` | **not ported** (no enemy of the 28 levels has a TRAJ) |
| `0x41c2e4` Wander action of kind 0 | 1 (`& 0x1fff`) | unreachable: the ctor gives every action kind 1 |

When C evaluates several draws in one expression (function arguments, `a * (b * c)`, an initializer list `{ x, y, z }`)
the order is unspecified and compilers differ, so such draws are split into one statement each, in the order of the
exe's calls.

## 3. What still differs

* **Order within a frame.** Each site draws as often as the original, but the port runs its systems in its own order
  (game messages, player, enemies, effects, storm, water, HUD, and the draw-time sites: water `Draw`, the HUD flight,
  the sky flash overlay), so draws of different systems in the same frame can interleave differently. The stream is
  the same, the numbers each site gets are not guaranteed to be.
* **Frame rate.** As in the original, per-frame and per-second draws depend on the frame times, so two runs only see the
  same numbers with the same frame times (`WOODY_FIXDT`); the original's own fixed step is `wverify.py --fixfps`.
* **Draws at render time** happen once per drawn frame: a frame drawn twice (a `--shot`) or not at all draws differently.
* **Pool limits.** Where a particle pool or the HUD ghost list (128) is full, the port skips the draws of the record it
  could not create; the original's pools are of other sizes.
* **ekorun / tools/ekovm.py** run the script alone: their `RANDOM` sees the stream from 1, the game's sees it after
  the 300 draws of the level load and everything drawn before. Message traces compared with the original agree only
  while a level's script draws nothing (House has no `RANDOM`).
