# EKO CODE – the script VM of Woody Woodpecker (PC, Eko Software 2001)

Everything below is derived from Woody.exe (build 17-10-2001) and verified with the
Python tools in `tools/`: the disassembler ([ekodisasm.py](../tools/ekodisasm.py)) parses all 28
`code` files without errors (all jump targets valid, each object ends cleanly), and the
emulator ([ekovm.py](../tools/ekovm.py)) runs the level initialization of all 28 levels with a
balanced stack and produces only message types the engine actually handles.

## 1. File format `Data\<LVL>\code`

Everything is little-endian uint32 ("word"). Indices below are word indices from the start.

| word | meaning |
|---|---|
| 0-1 | magic `EKO CODE` |
| 2 | `nobj` number of script objects |
| 3 | object table offset (always 12) |
| 4, 5 | `nvars`, variable table offset |
| 6, 7 | `nvol`, world_volume table offset |
| 8, 9 | `nstr`, string table offset |
| 10, 11 | `ncol`, world_collision table offset |
| 12 .. 12+nobj-1 | object table A: per object the code start (relative to the code base) |
| 12+nobj .. | **code base** B: the bytecode of all objects in sequence |
| vars_off | `nvars` entries of 2 words (value, runtime pointer), followed per variable by a list `[count][objId...]` = *watchers*: objects that run again when the variable changes |
| vol_off | `nvol` entries of 4 words (runtime: count u16, flags u16, timestamp, ptr) + per volume a watcher list `[count][objId...]` |
| str_off | `nstr` pointers (filled in at runtime) followed by C strings |
| col_off | `ncol` entries of 3 words + watcher lists |
| second-to-last | `0xFADEFADE` |
| last | compiler version, must be 6 (`"Conflit: La version du compilateur est %d..."`) |

Loader in the exe: `0x4424b0` (reads file, version check), `0x442570` (builds tables), `0x4427e0` (init).

## 2. Runtime model

- **Integer stack** (`0x5ce2b4`, sp at `0x5d051c`) and a separate **boolean stack** of bytes
  (`0x5ce44c`, sp at `0x5d0520`). Comparisons pop ints from the int stack and push a bool onto the bool stack;
  `JF` pops from the bool stack.
- **Globals** `0x5ce444 + 4*n`: global 0 = `this` (the actor id in a `FOREACH`), global 1 = free.
- **Variables** (per level) with watcher lists. `STOREVAR` wakes all watchers.
- **Time** `0x5d0514` in hundredths of a second: the engine sets per frame `time = (int)(frame_time_s * 100.0)` (`0x4019fd`, constant `0x4a9010`). `DELAY 100` = 1 second.
- Each object starts with `JMP <entry>`. During init, that `JMP` (2 words) is temporarily
  replaced by `NOP NOP` and the object is executed from word 0 (init block). Afterward the `JMP` is restored.
  If the object is later "woken", it starts again at word 0 and thus jumps straight to `<entry>`:
  for 8006 of the 13227 objects that's the end (object does nothing reactive), for 5221 it's a
  reactive block within the object.
- **Init** (`0x4427e0`) runs all objects **twice**; the messages of the first round are
  discarded (`0x441d40`), timers reset, then round two "for real".
  The messages of round two go into the **queue** (`0x5bd300`, max 1280) and are only handed to the
  game **after** init, just like every tick. That's not incidental: a handler that sets a script variable
  (`1082 LevelIsEnable` for the hub doors, GAMEFLOW.md §4.6) thereby wakes its watcher object, and at the end of
  `0x4427e0` all wake lists are cleared. If the port forwards them immediately during init, those
  wakes are lost and such an object never runs (`level_load` therefore forwards them after `eko_init`).

### Tick (`0x442240`, called from the game loop `0x4019c0`)
1. `0x442350`: execute all expired **DELAY** entries (sorted list `0x5d24d8..`, entry = {time, target}).
2. `0x4423a0` → `0x444150`: execute **all** DURING entries, every tick (unsorted list `0x5d0578..`); an entry whose time has expired (`time < now`) is first unlinked and then still runs one last time. `DURING d t` = "run t every tick, for d hundredths of a second" (verified live: the W2B boss intro sends `1152` every tick for 0.5 s).
3. `0x442320`: swap the double wake list (`0x4b3578`/`0x4b357c`, max 1000).
4. For each woken object (dedupe via per-object frame stamp `0x5d0550`): `run(codestart)`.
5. `0x4423f0`/`0x442450`: clear the per-frame flags of changed volumes/collisions.
6. Frame counter `0x4b3574`++, statistics (`"Total des during executes %d"` etc.).

Afterward the game loop processes the **outgoing message queue** (`0x5bd300`, 48-byte records, max 1280):
`record = {id, nargs, arg0..}`; arg0 is usually the target instance (`0x01000000 | index`).
`0x401a14..0x401a63`: count `0x441d20`, record i `0x441d30`, ids 1200..1300 to the app (`0x403440`), the rest to the
world (`0x401370`/`0x401250`), then `0x441d40` empties the queue. So a handler that writes a variable (1082, 1084, 1085, …)
does so only **after** every object of the tick has run: a later object in the same tick still reads the old value. The
port follows this (`main_engine.c`, `defer_msgs` around `eko_tick`); handing the messages over inside `SEND` broke the WWS
gate reveal (object 297 closed the gate with `3 [45, 0, 0, 10]` a tick early, and object 45's `3 [45, 0, 1, 1]` then opened it again).

## 3. Opcodes (handler table `0x5d0418`, init in `0x442a30`, interpreter `0x4429f0`)

Handler signature: `uint32* handler(uint32* pc)` returns the next pc. Opcode ≥ 63 stops.

| op | name | operands | semantics |
|---|---|---|---|
| 0 | NOP | | |
| 1 | HANG | | returns the same pc (unused) |
| 2 | END | | stop this run |
| 3 | PUSH | imm | |
| 4 | PUSHSTR | n | push pointer to string n |
| 5 | PUSHVAR | n | push var[n] |
| 6 | STOREVAR | n | var[n] = pop; wake watchers |
| 7-11 | ADD SUB MUL DIV NEG | | int stack |
| 12 | TOBOOL | | bpush(pop != 0) |
| 13-18 | EQ NE GT GE LT LE | | pop b, pop a → bpush(a ? b) (signed) |
| 19-21 | OR AND NOT | | bool stack |
| 22 | JMP | t | pc = t (absolute in B) |
| 23 | JF | t | if !bpop: pc = t |
| 24 | DELAY | d, t | schedule run(t) at time now+d |
| 25 | SKIP1 | x | nop with operand |
| 26 | DURING | d, t | run(t) **every tick** until now+d has elapsed (second list, see Tick step 2) |
| 27 | PUSHTIME | | push now |
| 28 | SEND | n | pop n values; message {id=first, args=rest} into the queue |
| 29,31,32,48,49 | VOL_FLAGb | v | bpush(bit 5/4/3/6/2 of volume[v].flags) |
| 30 | VOL_STATE | v | bpush(flags==0 or flags&9) |
| 33 | VOL_COUNT | v | push volume[v].count |
| 34 | FOREACH | v, end | for each actor in volume[v] without flag 1: this=actor; run(body); afterward pc=end |
| 35/36 | PUSHGLOBAL/STOREGLOBAL | n | |
| 37/38 | VOL_HAS / VOL_HASNOT | v, a | actor a in volume v (37: only if flags&4 and &0x24) |
| 39-42 | COL_FLAGb | c | bit 5/4/6/3 of collision[c].flags |
| 43 | JMPPOP | | pc = pop |
| 44 | VOL_SEQ | v1, v2 | bpush(vol[v2].time - vol[v1].time == 1) |
| 45/46 | VOL_ACTOR_F2/F1 | v, a | actor a in volume v with entry flag 2/1 |
| 47 | INVALID | | stop |
| 50 | VOL_ALL_F1 | v | all actors in v have flag 1 |
| 51/52 | COL_B3_BIT0 / COL_B2_BIT0 | c | |
| 53 | COL_ALL_F4 | c | |
| 54-56 | COL_ACTOR_F2/F4/F1 | c, a | |
| 57 | CUT | x | keyword `cut`, not implemented: bpush(0) + warning |
| 58 | MSGTEST | o | bpush(pop & msgmask[o]) |
| 59 | MSGCLEAR | o | msgmask[o] = 0 |
| 60 | VOL_PAIR | v1, v2, a | a in v1 with flag 1 and in v2 with flag 4 |
| 61 | DELAYPOP | t | schedule run(t) at now+pop |
| 62 | RANDOM | | push rand() % pop |

Handler addresses (op `0xaddr`, read from the init `0x442a30`; round 34): 0 `0x443210` · 1 `0x443200` · 3 `0x442cb0` · 4 `0x442cd0` · 5 `0x442d00` · 6 `0x442d30` · 7 `0x442d60` · 8 `0x442d90` · 9 `0x442dc0` · 10 `0x442df0` · 11 `0x442e20` · 12 `0x442e40` · 13 `0x442e80` · 14 `0x442ec0` · 15 `0x442f00` · 16 `0x442f40` · 17 `0x442f80` · 18 `0x442fc0` · 19 `0x443000` · 20 `0x443050` · 21 `0x4430a0` · 22 `0x4430f0` · 23 `0x4430c0` · 24 `0x443110` · 25 `0x4431d0` · 26 `0x443180` · 27 `0x4431b0` · 28 `0x443220` · 29 `0x4432a0` · 30 `0x443360` · 31 `0x443510` · 32 `0x443550` · 33 `0x443590` · 34 `0x443630` · 35 `0x4435d0` · 36 `0x443600` · 37 `0x4432e0` · 38 `0x4433c0` · 39 `0x443690` · 40 `0x443710` · 41 `0x4436d0` · 42 `0x4438a0` · 43 `0x4438e0` · 44 `0x443900` · 45 `0x443430` · 46 `0x4434a0` · 48 `0x443a00` · 49 `0x443a40` · 50 `0x443a80` · 51 `0x443ad0` · 52 `0x443b10` · 53 `0x443b40` · 54 `0x443750` · 55 `0x4437c0` · 56 `0x443830` · 57 `0x443b90` · 58 `0x443bd0` · 59 `0x443c20` · 60 `0x443960` · 61 `0x443140` · 62 `0x443c50`. Op 2 gets a register value (`0x442b1e`, the end-of-run marker) and op 47 has no entry (both stop the run). The condition opcodes push onto the byte stack `0x5ce44c` (top `[0x5d0520]`), the int stack is `0x5ce2b0` (top `[0x5d051c]`). The VM's node free list is `0x5d6378` (init `0x444780`, alloc `0x4447b0`). Port: `src/ekovm.c`, message stream checked identical against the original (TRACING.md).

## 4. Message routing (engine side, `0x4019c0` → `0x401370`)

| id range | handler | meaning |
|---|---|---|
| 1200-1300 | `0x403440` | 1200 = **SetTypeInstance(obj, type)**: `new` of the C++ class for `type` (table at `0x403502`, 121 types → 42 classes). 1201/1202 = set/clear flag 0x400 |
| < 1000 | `instance->vtable[22](record)` | per-class message handler; shared base `0x42d5e0` (ids 1..56, 22 cases), class-specific ids inline (e.g. Perso 26/30, enemies 6/11) |
| 7 | `0x4012f0` | cancel pending messages 12/13 for that object |
| 1000-1499 | `0x444870` (this `0x5d7afc`) | game/level messages, 46 cases for 1000..1180 (incl. LevelIsEnable, SaveAuto, SetRaceInfo, StartBoostSurf) |
| 1500-1599 | `0x46cca0` (this `0x5e823c`) | 12 cases (position/vector-like, floats × scale) |
| 1600-1700 | `0x467fa0` (this `0x4c2dd8`) | 58 cases, sound (vtable calls with volume 1.0) |

Handlers that return `true` are retried (`0x401250`, list of 32 deferred records).

Reference encoding in arguments: `0x01000000 | i` = instance i (in `[0x50944c]->0x6c[i]`), `0x02000000 | i` = script variable i (FORMAT_INS.md §5), `0x0002xxxx` pairs occur as (type, index).

## 5. Engine → VM

Callback table `0x5cc360[id]` (dispatcher `0x441c90`), ids 100-103 set in `0x441ed0`:
100 = SetVar(var, value), 101 = volume **Enter**(vol, actor), 102 = **Leave**, 103 = **In**.
Collision variants (Press/UnPress/In/PersoUnpress) via `0x441fc0..0x442100`.
Each event sets flags on the volume and on the actor entry and wakes the watcher objects of that volume (`0x443d20`).
Source of the volume events: `0x430210` (bounding-volume test per actor, `push 0x65/0x66/0x67`).

## 6. Implementations

- `tools/ekovm.py`: Python emulator (init + tick), used for statistics and as a reference.
- `src/ekovm.c` + `src/ekovm.h`: C implementation with the same semantics (including the quirks:
  double init pass, `FOREACH` terminates the enclosing run, wake list reversed, queue cap of 1280
  messages with a warning). `src/ekorun.c` is a test harness; traces are identical to the Python emulator.
- Both use the MSVC `rand()` LCG (`seed*214013+2531011`, seed 1) so `RANDOM` is deterministically comparable; the C VM
  draws from `crt_rand()` (`src/crtrand.h`), the one stream the whole game shares, as in the exe (docs/RANDOM.md).

## 7. What's still missing for a 1:1 reimplementation

Nothing for the VM itself: every message id the 28 level scripts send is handled by the port (MESSAGES.md "Coverage of the
port"), the VM tick `0x4019c0` is called from the game frame `0x401ab0` and from `0x404822`, and the `0x02000000 | i`
references are script variables (FORMAT_INS.md §5, CAMERA_SCRIPT.md), `0x01000000 | i` instances and `0x03000000 | i`
volumes (EVENTS.md). What is still open lives in the engine classes the messages drive; see TODO.md.
