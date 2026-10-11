# Enemy class 14 – Buzz Buzzard (end boss W1B, W2D, W3D, WWS) – Woody.exe

Status: static analysis of `game/Woody.exe` (`out/disasm_full.txt`, `tools/drange.py`), fully read from `0x40eb50` through `0x410e90`
(ctor up to and including `vtbl[31]`). Floats, jump tables and animation records were extracted from the exe with a PE reader, not guessed. Follows on from
ENEMY.md / ENEMY2.md: the base class `Enemy`, the parameter block **P**, the angle controller **H**, the behaviours (Wander / Chase /
Stand), `Enemy::Update 0x41a3e0`, `Enemy_TakeDamage 0x41adc0`, `FindTarget` and `AnimLen(n,k) = 0x436b90` apply unchanged here.
"uncertain" = cannot be determined statically.

## 0. Summary

* **Class factory** `0x403502`: type 14 → `new(0x24c)` + `0x40eb50(11)` (`push 0xb` at `0x403883`) → **subtype 11**; then (as with
  every type) `vtbl[1]` PostLoad, `vtbl[17]` Reset and `0x407790` (hangs in the world) (`0x403e6d..0x403e7a`).
* **Two variants** ("mode" `+0x228`), chosen by the **script via a mailbox variable** (message 60): mode **1** = W1B (Buzz in his
  flying machine, logical records 0..16), mode **2** = W2D / W3D / WWS ("bobbing" with dust). Mode **0** = off: the class
  then does nothing at all except read the mailbox.
* **Fight** (mode 1): Buzz chases the player **high up** (at the y of his placement, W1B 2430) at 900 u/s, shakes for 0.25 s above the player and
  then **falls** with gravity (stomp; hits the player within a cone = 1 heart; camera shake on landing). Afterwards he hovers
  **low** (ground below the start position + 290, W1B ≈ 1635) and is **only then vulnerable**: every hit = **1 hp** (regardless of damage),
  **5 hp**, 2.1 s flashing red "hit", then back up. If he hits the player during the low phase, he also goes back up.
* **Linked instance** (message 59, W1B: 404 = type 17, model 38): gets **exactly the position and rotation** of the boss every frame, plays
  **the same logical animation** (own AnimCtrl, own model), flashes red along with him, and gets smoke plumes on its markers at hits 2..4.
* **End**: at hp ≤ 0 he writes **3** to the mailbox variable (W1B: var 51), stops his sound and sets mode 0 (freezes). The script
  hides 405/404 0.5 s later and starts the outro. Otherwise he only ever writes **−1** (acknowledgement of a command), never 1 or 2.
* **HUD**: every frame `0x4484d0(1, (int)hp, (int)P+0x34)` = boss health bar (HUD_TEXT.md §4.4) with 5 dots.
* **No** projectiles, no bombs, no TRAJ/path, no `vtbl[58]`, no death particles, no fade, no removal (`+0x10c` stays 0).

## 1. Vtable `0x4a98a8` (58 slots) vs. the Enemy base `0x4a9fbc`

Slots 0..57 as in ENEMY.md §1.1. After slot 57 there are three floats (`0x4a9990` = 1/60, `0x4a9994` = 20.0, `0x4a9998` = 500.0) and then the
AnimCtrl vtable `0x4a999c` (`[0]` = `0x4108e0`, `[1]` `0x40d7f0`, `[2]` `0x436b70` Request, `[3]` `0x436a50` Tick, `[4]` `0x436a40` Reset).
So there is **no slot 58 (Fire)**.

| slot | off | class 14 | base | meaning |
|---|---|---|---|---|
| 0 | +0x00 | `0x40ebc0` | `0x419d30` | scalar dtor → `0x40ebe0`: both AnimCtrls (`+0x1c4`, `+0x1c8`) `vtbl[1](1)`, then `0x419d50` |
| 1 | +0x04 | **`0x40ec50`** | `0x419e30` | PostLoad (§3.2) |
| 3 | +0x0c | = `0x41a320` | | Think: Update only if the 3D distance to the camera `< P+0xc0` (**3000**, not overridden) or `hp ≤ 0`; not during a cinematic |
| 17 | +0x44 | **`0x40ed90`** | `0x41a010` | Reset (§3.3) |
| 22 | +0x58 | **`0x410070`** | `0x41a740` | messages 59 / 60, rest → `Enemy::HandleMsg` (§7) |
| 23 | +0x5c | `0x45b340` (`return 8`) | `0x41ad40` (1 or 0x10) | no caller found with certainty: uncertain |
| 26 | +0x68 | **`0x40fde0`** | `0x430010` | render colour: red/white flashing in states 9 and 12 (§8) |
| 31 | +0x7c | **`0x410cf0`** | `0x40c1e0` | **Touch** = cone test (§6.1) |
| 38 | +0x98 | `0x4078a0` (empty) | `0x40c3b0` (empty) | |
| 39 | +0x9c | **`0x40fe90`** | `0x41adc0` | **TakeDamage** (§6.2) |
| 40 | +0xa0 | = `0x41ae20` | | Blast: goes via `vtbl[39]` ⇒ also just 1 hp |
| 43 | +0xac | **`0x410900`** | `0x41a4e0` | height control / falling (§5) |
| 45 | +0xb4 | **`0x410170`** | purecall | animation choice (§4.2) |
| 47 | +0xbc | = `0x41a4d0` | | always registered as actor/attack target |
| 51 | +0xcc | **`0x4105e0`** | purecall | `return 4.0f` (`0x4a94c0`); only used by the fade of `Enemy::Update`, which never starts (`+0x15c` stays 0) |
| 52 | +0xd0 | **`0x40eec0`** | `0x41a3e0` | Update / state machine (§3A) |
| 53 | +0xd4 | **`0x4105f0`** | `0x40d840` | duration per state (§4.3) |
| 54 | +0xd8 | **`0x40fe80`** | `0x4078b0` (0) | `return +0x234` = **linked instance** |
| 55 | +0xdc | = `0x41b1b0` | | **drag along**: if `vtbl[54]()` ≠ 0 ⇒ copy position and rotation to the linked instance (§9.1). Correction to ENEMY.md §1.1 ("update cell") |
| 57 | +0xe4 | = `0x41b000` | | death effect – **never** called in this class |

All other slots = base (among others 32 radius = `P+4`, 33 height = `P+0x28`, 34 position pointer, 37 warning `ret 4`, 41 = `0x4078a0` empty,
48 FindTarget).

## 2. Fields (size 0x24c)

### 2.1 Own fields

| off | type | init | meaning | writers / readers |
|---|---|---|---|---|
| 0x1c0 | int | Reset 0 | **state** 0..12 (13 only in the anim/duration table) | |
| 0x1c4 | AnimCtrl* | ctor 0; PostLoad | own AnimCtrl (`0x4108c0(this)`, 0x54 B) | |
| 0x1c8 | AnimCtrl* | ctor 0 | AnimCtrl of the **linked instance** (`0x4108c0(link)`, message 59) | `0x410126` |
| 0x1d0 | float | – | time the player stands still (+= dt in state 3, 0 if he moves) | **no reader** |
| 0x1d4 | float | – | shake timer state 6 (`P+0xa4` = 0.25) | prologue `-= dt` while ≥ 0 |
| 0x1d8 | float | Reset 0 | wait timer after landing, state 8 (`P+0x9c` = 0.3) | ditto |
| 0x1e0 | float | – | cheer timer state 10/11 (4.0) | |
| 0x1e4 | float | – | timer state 1 (`P+0x38` = 1.0 or 0) | prologue `-= dt` while ≥ 0 |
| 0x1e8 | int | – | step counter 0..3 of the shaking (state 6) | |
| 0x1ec | vec3 | – | player position previous frame | state 2 sets 0 |
| 0x1f8 | vec3 | – | player position this frame | |
| 0x204 | vec3 | – | saved home point during the retreat | |
| 0x210 | float | – | saved `P+0x1c` (leash) during the retreat | |
| 0x214 | float | PostLoad | **high hover height** = y of the home point = y of the placement (W1B **2430**) | |
| 0x218 | float | PostLoad | **low hover height** = ground below the placement (`0x41a2a0`) + `P+0x88` (290). W1B: world floor below (−7031, −8863) = **1344.8** ⇒ **1634.8** (determined with `tools/gelparse.py` over the world polygons, without instance hulls) | |
| 0x21c | float | Reset 0 | accumulator for the fixed 60 Hz steps of state 6 | |
| 0x224 | u8 | Reset 1 | **"high"**: 1 = high phase (invulnerable), 0 = low phase (vulnerable) | |
| 0x228 | int | PostLoad 0 | **mode** 0 = off, 1 = W1B variant, 2 = W2D/W3D/WWS variant | `0x410be0` |
| 0x22c | int | Reset 0 | no reader | |
| 0x230 | int | PostLoad 0 | **mailbox variable** (script var id, `& 0xffffff`), message 60 | |
| 0x234 | Inst* | ctor 0 | **linked instance** (message 59) | |
| 0x23c | u8 | Reset 1 | mode 2: direction of the bobbing (1 = down) | |
| 0x240 | float | Reset 0 | mode 2: bob depth 0..150 | |
| 0x244 | float | Reset 150.0 (`0x43160000`) | mode 2: maximum bob depth | |
| 0x248 | Source (4 B) | Reset `0x468e10` | sound source for the loop (SOUND.md §5: `0x468e40` active, `0x468e50` start, `0x468e20` stop) | |

### 2.2 Used base fields

`+0xc` position (**the class moves this itself**, §5), `+0x114` dt, `+0x118` P, `+0x11c` active behaviour, `+0x120` H, `+0x128` start position,
`+0x134` home point (`+0x138` = y), `+0x150` hp, `+0x158` hit timer, `+0x15c` death timer (always 0), `+0x160` Wander, `+0x164` Chase,
`+0x168` Stand (**no** Path: `+0x16c` stays 0, TRAJ is not used), `+0x174` flags (bit 1 on the ground, **bit 4 = gravity/falling on**),
`+0x178` ground probe.

### 2.3 Parameter block P for subtype 11 (`0x41d510`, case `0x41daa6`)

| P+ | value | mode 1 (Reset) | mode 2 (Reset) | used in this class |
|---|---|---|---|---|
| 0x00 | **200** | (400) | (800) | fall acceleration: the Fall object (`+0x110`, ctor `0x4402f0`) copies it **once**, in PostLoad (`0x419eb2`), so the drop always uses **200**; Reset's 400 / 800 have no reader (§3.3) |
| 0x04 | **240** | | | radius (`vtbl[32]`; used by other code, not by the class) |
| 0x08 | **600** | **400** | **390** | walk speed (Wander, retreat, H speed in state 10) |
| 0x0c | **900** | **900** | **400** | run speed (Chase); mode 2 also bob speed |
| 0x10 | **π/4** | | | turn speed Wander / retreat |
| 0x14 | **π** | | | turn speed Chase, state 5 |
| 0x18 | π/2 | | | not read |
| 0x1c | **1000** | | | leash around the home point (3D for subtype ≥ 9); during the retreat 1.0 |
| 0x20 | **4600** | | | FindTarget sight distance (3D) |
| 0x24 | **3000** | | | FindTarget max. \|dy\| |
| 0x28 | **150** | | | height (`vtbl[33]`; h/2 = 75 for probe and cell) |
| 0x2c / 0x30 | **15000** | | | max. step-down / step-up ⇒ no edge test |
| 0x34 | **5** | | | **hit points** (and HUD maximum) |
| 0x38 | 1.0 | | | duration state 1 in the high phase |
| 0x3c | 1.0 | | | **damage to the player** (cone contact) |
| 0x78 | **900** | | | vertical speed of the height control |
| 0x7c | **250** | | | **cone height** (below `pos`); hit point at `pos.y − 125` |
| 0x80 | **150** | | | **cone radius** at the top / hit-point distance |
| 0x88 | **290** | | | low hover height above the ground |
| 0x9c | **0.3** | | | wait time after landing (state 8) |
| 0xa4 | **0.25** | | | shake duration (state 6) |
| 0xa8 | **20** | | | shake amplitude |
| 0x84, 0xa0 | 2.0, 2.0 | | | set, **no reader** |
| 0xc0 | 3000 | | | activation distance to the camera (Think) |

Other fields = defaults (ENEMY.md §2.3; among others `P+0x44 = 600` knockback factor). Note: Reset only overwrites `P+0/8/0xc` if the
mode is 1 or 2; the Reset from the factory (mode 0) leaves 200/600/900 as is.

## 3. Construction, PostLoad, Reset, mailbox

### 3.1 Ctor `0x40eb50(subtype)` and dtor

```c
Boss14 *Boss14_ctor(Boss14 *e, int subtype) {        /* 0x40eb50 */
    Enemy_ctor(e);                                    /* 0x419cd0 */
    e->vtbl = 0x4a98a8;  Npc_SetCategory(e, 2);  Npc_SetSubtype(e, subtype /*11*/);   /* 0x40c360, 0x40c380 */
    e->anim = NULL; e->linkAnim = NULL; e->link = NULL;   /* +0x1c4, +0x1c8, +0x234 */
    return e;
}
void Boss14_dtor(Boss14 *e) {                         /* 0x40ebe0 */
    if (e->anim) e->anim->vtbl[1](1);  if (e->linkAnim) e->linkAnim->vtbl[1](1);  Enemy_dtor(e);  /* 0x419d50 */
}
```

### 3.2 PostLoad `0x40ec50`

```c
void Boss14_PostLoad(Boss14 *e) {
    Enemy_PostLoad(e);                                /* 0x419e30: P, H, Sensor, Fall, start/home = pos */
    e->anim   = new AnimCtrl14(e);                    /* 0x4108c0: 0x4369f0(e) + vtable 0x4a999c */
    e->wander = new Wander(e, 1, &e->home);           /* +0x160, 0x41bf30: leash around +0x134 */
    e->yHigh  = e->home.y;                            /* +0x214 */
    e->yLow   = GroundBelow(e) + P->+0x88;            /* +0x218 = 0x41a2a0() + 290 */
    e->chase  = new Chase(e, 0);               /* +0x164, 0x41bbf0 */
    e->still  = new Stand(e);                /* +0x168, 0x41b710 */
    e->mailVar = 0;  e->mode = 0;                     /* +0x230, +0x228 */
}
```
No Path behaviour and no `[0x4c5330]++` (does not count as "enemy in the level").

### 3.3 Reset `vtbl[17]` = `0x40ed90`

```c
void Boss14_Reset(Boss14 *e) {
    Enemy_Reset(e);            /* 0x41a010: pos = home = START +0x128 (not where he is now), H angle snapped to the start angle
                                  H+0x1c and its angular speed 0 (0x41b840), Fall object cleared (0x440310: v and clock, NOT its g),
                                  hp = P+0x34, hitT = deathT = 0, type word |= 0x400, not in the world ⇒ 0x407790 (so becomes
                                  visible again!), flag 4 on, clear msgmask 0x10. It touches no behaviour (knock timers, §6.2) */
    e->behav = e->wander;  e->wander->vtbl[6]();  Wander_Start(e->wander);   /* vtbl[6] 0x41c090 → 0x41b2b0: Wander knock timer 0; 0x41c160: turn speed P+0x10 */
    e->flags &= ~4;                                    /* no falling */
    e->state = 0;  e->t1d8 = 0;  e->deathT = 0;  e->high = 1;  e->u22c = 0;  e->acc = 0;
    e->bobDown = 1;  e->bobMax = 150.0f;  e->bob = 0;
    e->hp = P->+0x34;                                  /* 5 */
    smokeOn[0] = smokeOn[1] = smokeOn[2] = 0;          /* bytes 0x5e857c..e: smoke plumes off (§9.2) */
    e->anim->vtbl[4]();  if (e->linkAnim) e->linkAnim->vtbl[4]();   /* AnimCtrl reset (0x436a40) */
    switch (e->mode) {
    case 1: P->+0x0c = 900; P->+0x08 = 400; P->+0x00 = 400; break;     /* 0x40ee7d; P+0 has no reader any more (§2.3) */
    case 2: P->+0x0c = 400; P->+0x08 = 390; P->+0x00 = 800; break;     /* 0x40ee49; ditto: the drop keeps g = 200 */
    }
    SoundSrc_Init(&e->src);                            /* 0x468e10(this+0x248) */
}
```
Reset is called by the factory (message 1200), by **every mode switch** (§3.4) and by message `11 [inst, 4]` (base; W2D/W3D
use that to put the boss back when Woody loses a life in the arena, §11.1).

### 3.4 Mailbox `0x410be0` (every Update, first thing)

```c
bool Boss14_CheckCommand(Boss14 *e) {
    int v = *GetVar(e->mailVar);                       /* 0x443cd0 */
    if (v == 1)      { if (e->mode != 1) { e->mode = 1; e->vtbl[17](); } SetVar(e->mailVar, -1); return true; }
    else if (v == 2) { if (e->mode != 2) { e->mode = 2; e->vtbl[17](); } SetVar(e->mailVar, -1); return true; }
    SetVar(e->mailVar, -1);                            /* 0x443ca0; also on any other value (0, 3, -1) */
    return false;                                      /* result is not used by Update */
}
```
* The script writes **1 or 2** (command "start in mode 1/2"); the boss acknowledges with **−1**. A command for the mode that is already active does
  nothing (no Reset). SetVar wakes the watchers before writing (`0x443ce0`), so every Update wakes the script objects waiting on the var.
* As long as message 60 has not been received, `mailVar = 0`: the boss then reads **var 0** and writes −1 into it (only if it is already being
  updated in those first frames, i.e. while within 3000 of the camera).
* The mailbox is only read if Think calls Update (camera < 3000, no cinematic).

## 3A. Update `vtbl[52]` = `0x40eec0` (jump table `0x40fd9c`, 13 states)

```c
void Boss14_Update(Boss14 *e) {
    Boss14_CheckCommand(e);                                        /* 0x410be0 */
    if (e->mode == 0) return;                                      /* 0x40eee8 → 0x40fd8a: also no HUD */
    Enemy_Update(e);   /* 0x41a3e0: behaviour tick, vtbl[43] height (§5), vtbl[44] rotation, vtbl[45] animation (§4), vtbl[55] drag along (§9.1) */
    if (e->mode == 1 && e->link && e->link->smoke) e->link->smoke->on = 1;   /* 0x40ef08: type-17 smoke (+0x10c)->+0xc; W1B: smoke == 0 */
    if (e->state == 3) e->stillT += dt;
    if (e->t1d4 >= 0) e->t1d4 -= dt;   if (e->t1d8 >= 0) e->t1d8 -= dt;   if (e->t1e4 >= 0) e->t1e4 -= dt;
    Actor *t;                              /* FindTarget = vtbl[48](1, 0): sight 4600 (3D), |dy| < 3000 */
    switch (e->state) {
    case 0: /* TO HOVER HEIGHT / WANDER START  0x40efb5 */
        e->home.y = e->high ? e->yHigh : e->yLow;
        e->flags &= ~4;
        e->behav = e->wander;  e->wander->vtbl[6]();  Wander_Start(e->wander);
        e->t1e4 = e->high ? P->+0x38 /*1.0*/ : 0;   e->state = 1;
        break;
    case 1: /* WANDER  0x40f045 */
        if (e->t1e4 < 0) e->state = 2;
        break;
    case 2: /* NOTICE  0x40f06b */
        if (!(t = FindTarget(1,0))) { e->state = 0; break; }
        e->flags &= ~4;  H->speed = P->+0x08;                      /* immediately overwritten by Chase_Start */
        e->behav = e->chase;  e->chase->vtbl[6]();  Chase_Start(e->chase, t);   /* 0x41bc80: speed P+0xc (900) immediately, turn P+0x14 (π) */
        e->stillT = 0;  e->plPrev = (0,0,0);  e->state = 3;
        break;
    case 3: /* CHASE  0x40f0e9 */
        if (e->high && e->home.y - e->pos.y >= 20.0f) break;        /* 0x4a9994: wait until (nearly) at the high hover height */
        if (!(t = FindTarget(1,0))) { e->state = 0; break; }
        e->plPrev = e->plCur;  e->plCur = t->pos;
        if (!PlayerStill(&e->plPrev, &e->plCur)) e->stillT = 0;     /* 0x410c60: |Δ| < 1.0 */
        if (!e->high) {
            if (e->vtbl[31](1, 0)) {                                /* cone contact §6.1 */
                vec3 d = normalize_xz(t->pos - e->pos);
                vec3 pt = { e->pos.x + d.x*P->+0x80, e->pos.y - P->+0x7c*0.5f, e->pos.z + d.z*P->+0x80 };   /* pos + d·150 − (0,125,0) */
                if (t->vtbl[39](e, P->+0x3c /*1.0*/, &d, &pt, 0)) { e->state = 10; break; }
                SoundFx(e->mode == 1 ? 41 : 46, 0);                 /* 0x40f291 */
                e->high = 1;  e->state = 0;  break;                 /* back up */
            }
            if (dist_xz(e->pos, e->plCur) < 150.0f) {               /* 0x4a9754; player below/beside him but not in the cone ⇒ RETREAT */
                vec3 a = e->home - t->pos;  float L = len3(a);      /* 3D length, but only x/z are used */
                e->homeSave = e->home;
                e->home = (vec3){ t->pos.x + a.x/L*1300.0f, e->yLow, t->pos.z + a.z/L*1300.0f };   /* 0x4a9860 */
                e->behav = e->wander;  e->wander->vtbl[6]();  Wander_Start(e->wander);  Wander_SetLeash(e->wander, 1, &e->home);
                e->leashSave = P->+0x1c;  P->+0x1c = 1.0f;  e->state = 4;  break;
            }
        }
        if (dist_xz(e->pos, e->plCur) < 150.0f) {                   /* 0x40f407 */
            e->behav = e->still;  e->t1e8 = 0;  e->t1d4 = P->+0xa4 /*0.25*/;
            if (e->high) e->state = 6;                              /* STOMP run-up */
        }
        break;
    case 4: /* RETREAT  0x40f47d */
        if (!(t = FindTarget(1,0))) goto restore;
        e->plPrev = e->plCur;  e->plCur = t->pos;
        if (!PlayerStill(&e->plPrev, &e->plCur)) goto restore;      /* player moves ⇒ abort */
        Wander_SetLeash(e->wander, 1, &e->home);
        if (dist3(e->pos, e->home) < 150.0f) e->state = 5;
        break;
    case 5: /* WAITING AT THE RETREAT POINT  0x40f558 */
        if (!(t = FindTarget(1,0))) goto restore;
        e->plPrev = e->plCur;  e->plCur = t->pos;
        if (!PlayerStill(&e->plPrev, &e->plCur)) goto restore;
        e->behav = e->still;  H_SetTurnSpeed(H, P->+0x14 /*π*/);  H_TurnTo(H, e->pos, t->pos, 0);   /* 0x41b940, 0x41ba60 */
        if (AngArc(H->target, H->angle > 0 ? 1.0f : 0.0f) != 0) H_Tick(H, dt);   /* 0x4401c0 – literally like this (presumably intended:
                                                                                     arc(target, angle)); in practice he always turns */
        break;
    restore: /* 0x40f66f */
        P->+0x1c = e->leashSave;  e->home = e->homeSave;  Wander_SetLeash(e->wander, 1, &e->home);  e->state = 2;
        break;
    case 6: /* SHAKING ABOVE THE PLAYER  0x40f6bb */
        if (!(t = FindTarget(1,0))) { e->state = 2; break; }
        if (e->t1d4 <= 0) { e->state = 7; e->flags |= 4; break; }   /* gravity on ⇒ falling */
        e->acc += dt;  if (e->acc <= 1/60.0f) break;                /* 0x4a9990: fixed step of 1/60 s */
        vec3 p = e->pos;  vec3 d = normalize_xz(H_MoveDir(H));      /* 0x41b860 */
        int n = e->t1e8++;
        switch (n) { case 0: s = -20; break;  case 1: case 2: s = +20; break;  case 3: e->t1e8 = 0; s = -20; break; }   /* table 0x40fdd0, P+0xa8 */
        p += d * s;                                                  /* pattern −20, +20, +20, −20 along the movement direction */
        if (e->t1e8 & 1) { vec3 w = t->pos - p; w.y = 0; if (len_xz(w) != 0) p += w * 0.1f; }   /* 0x4a9008: 10 % towards the player */
        e->pos = p;
        while (e->acc > 1/60.0f) e->acc -= 1/60.0f;                  /* at most one step per frame, remainder is dropped */
        break;
    case 7: /* FALLING (STOMP)  0x40f909 */
        if (!(t = FindTarget(1,0))) { e->state = 0; break; }
        if (e->flags & 1) {                                          /* landed (base ground follower, §5) */
            CameraShake(cam, 1.5f);                                  /* 0x41fbb0 */
            e->high = 0;  e->t1d8 = P->+0x9c /*0.3*/;  e->state = 8;
            if (e->mode == 2) Dust(&(vec3){pos.x, pos.y - 50, pos.z}, &(vec3){0,1,0}, 0, 1.5f, 6.0f);   /* 0x476140: smoke ring kind 0, PARTICLES.md 3 */
            SoundFx(e->mode == 1 ? 40 : 45, 0);                      /* 0x40f9e5 */
        }                                                            /* no break: falls through */
        H_TurnTo(H, e->pos, t->pos, 0);  H_Tick(H, dt);
        e->pos.x += (t->pos.x - e->pos.x) * 0.01f;  e->pos.z += (t->pos.z - e->pos.z) * 0.01f;   /* 0x4a94f8: 1 % per FRAME */
        if (e->vtbl[31](1, 0)) {
            vec3 d = normalize_xz(t->pos - e->pos), pt = e->pos + d*150 − (0,125,0);
            if (t->vtbl[39](e, 1.0f, &d, &pt, 0)) { e->state = 10; break; }
            SoundFx(e->mode == 1 ? 41 : 46, 0);  CameraShake(cam, 1.5f);   /* 0x40fb78, 0x40fb88 */
            e->high = 0;  e->t1d8 = 0.3f;  e->state = 8;
        }
        break;
    case 8: /* AFTER LANDING  0x40fbb5 */
        if (e->t1d8 < 0) e->state = 0;                               /* low phase begins (high == 0) */
        break;
    case 9: /* HIT  0x40fbd1 */
        if (e->hp <= 0) { e->state = 12; break; }
        e->hitT -= dt;  e->behav = e->still;
        if (e->hitT < 0) { e->high = 1; e->state = 0; }               /* 0x40fd4e: back up */
        break;
    case 10: /* PLAYER DEFEATED start  0x40fcb1 */
        if (e->mode == 1) { SoundFx(43, 0); SoundSrc_Stop(&e->src, 0, 39); } else { SoundFx(48, 0); SoundSrc_Stop(&e->src, 0, 44); }
        e->t1e0 = 4.0f;  e->state = 11;  e->behav = e->still;  H_SetSpeed(H, P->+0x08, 1);   /* 0x41b9d0 */
        /* fallthrough */
    case 11: /* CHEERING  0x40fd2f */
        if ((e->t1e0 -= dt) <= 0) { e->high = 1; e->state = 0; }
        break;
    case 12: /* DEFEATED  0x40fc27 */
        e->behav = e->still;  e->flags |= 4;
        if (e->hitT >= 0) e->hitT -= dt;
        SetVar(e->mailVar, 3);                                        /* 0x40fc6f: ⇒ script "boss defeated" */
        SoundSrc_Stop(&e->src, 0, e->mode == 1 ? 39 : 44);            /* 0x468e20 */
        e->mode = 0;                                                  /* from the next frame on, Update does nothing anymore */
        break;
    }
    HUD_BossBar(1, (int)e->hp, (int)P->+0x34);                        /* 0x40fd82: 0x4484d0 on [0x5d7b44] */
}
bool PlayerStill(vec3 *a, vec3 *b) { return len3(*a - *b) < 1.0f; }  /* 0x410c60 */
```

### 3A.1 States at a glance

| # | code | name | behaviour | to |
|---|---|---|---|---|
| 0 | `0x40efb5` | to hover height | Wander | → 1 |
| 1 | `0x40f045` | wander | Wander | `t1e4 < 0` → 2 (high: after 1.0 s, low: next frame) |
| 2 | `0x40f06b` | notice | → Chase | target → 3, else → 0 |
| 3 | `0x40f0e9` | chase (900 u/s, turning π) | Chase | high: xz < 150 → **6**; low: cone contact → player hit → 0 (high) or 10; xz < 150 without contact → **4**; no target → 0 |
| 4 | `0x40f47d` | retreat to a point 1300 from the player | Wander (leash 1.0) | player moves → 2; within 150 of the point → 5 |
| 5 | `0x40f558` | hang still, turn to the player | Stand | player moves / gone → 2 |
| 6 | `0x40f6bb` | shaking above the player (0.25 s) | Stand | → 7 (fall on) |
| 7 | `0x40f909` | falling, 1 %/frame sliding towards the player | Stand | landed or contact → 8 (low); player dead → 10 |
| 8 | `0x40fbb5` | after landing (0.3 s) | Stand | → 0 (low) |
| 9 | `0x40fbd1` | hit (2.1 s) | Stand | hp ≤ 0 → 12; `hitT < 0` → 0 (high) |
| 10/11 | `0x40fcb1` / `0x40fd2f` | player defeated, 4.0 s | Stand | → 0 (high) |
| 12 | `0x40fc27` | defeated: var := 3, mode := 0 | Stand | (frozen) |

**Cycle**: 0 → 1 (1 s) → 2 → 3 (high chase) → 6 (0.25 s shaking) → 7 (fall) → 8 (0.3 s) → 0 → 1 → 2 → 3 (low chase;
vulnerable) → { peck ⇒ 9 (2.1 s) ⇒ 0 high | contact ⇒ player −1 ⇒ 0 high | player stands still nearby ⇒ 4/5 retreat until he moves ⇒ 2 }.
The actual "fall in" condition for the stomp is only **xz distance < 150** (plus ≤ 20 below the high hover height); there is no line-of-sight or angle test.
Frame-dependent (literally): the 1 % slide in state 7, the fall (per frame, §5) and the at-most-one-shake-step-per-frame in state 6.
The port normalises the 1 % slide to 60 Hz (`1 − 0.99^(60·dt)`, like its other per-frame effects) and keeps the fall's literal per-frame
step like every other ground follower (its terminal speed `g/0.2` = 1000 u/s does not depend on the frame rate, only the first frames do).

**Knock timers.** Every behaviour switch of states 0, 2 and 3 (and Reset) calls the new behaviour's `vtbl[6]` first (`0x40eff6`,
`0x40f0af`, `0x40f3c6`, `0x40eda7`: Wander `0x41c090`, Chase `0x41bc60`), and both start with `0x41b2b0`, which zeroes the
behaviour's knock timer `Behav+0x1c` (and `+0x20..+0x28`). The switches to Stand (states 3, 5, 9, 10, 12) do not. So the knock
timer a peck puts on the Chase (§6.2, `AnimLen(8/25)`) is frozen through state 9 (Stand is active) and the wander, and cleared by the
next chase start in state 2. Without that clear (the port before this was fixed) the chase spends the leftover 2.1 / 2.4 s with a zero
direction and a step > 0, so the Chase's stuck hook `0x41bdf0` (moved < 0.01 ⇒ a random new target angle, ENEMY.md §5.1) fires every
frame: Buzz spins in place after rising from a hit, and a peck while the timer still runs costs no hp.

## 4. Animation

### 4.1 AnimCtrl `0x4108c0` and records

`0x4108c0(inst)` = `0x4369f0(inst)` + vtable `0x4a999c`; record getter `0x4108e0(n)` = `0x4b1958 + n·0x1c` (34 records, ending exactly where the
type-12 table `0x4b1d10` begins), format `{int sub[4]; int prio; float speed; u8 restart}`; all prio 1000, restart 1. `AnimLen(n) = duration(sub[0]) / speed`
of the **boss model** (`AnimCtrl+0x4c` = the boss). Duration in s of W1B model 37 (43 anims; measured from the .ins): 5 = 5.9, 6 = 1.0, 7 = 0.5, 8 = 1.8,
9 = 0.9, 10 = 0.1, 11 = 0.9, 12 = 1.3, 13 = 6.6, 14 = 2.1, 15 = 2.1, 16 = 4.3, 17 = 5.9, 18 = 4.6; 0/1 = 59.6/54.8 (cinematic tracks), 2/3/4 = 10.0.

| n | sub[] | speed | mode | used in state | AnimLen W1B |
|---|---|---|---|---|---|
| 0 | 5,5,5,5 | 3 | 1 | – (unused) | 1.97 |
| 1 | 6,7,7,7 | 3 | 1 | – | 0.33 |
| **2** | 7,7,7,7 | 3 | 1 | **movement**: wander actions 5,6,7,9,10; states 2, 3 | 0.167 |
| 3 | 8,5,−1,−1 | 3 | 1 | – | 0.6 |
| **4** | 9,10,10,10 | 3 | 1 | **6, 7** shaking + falling | 0.3 (then 10 looped) |
| 5 | 11,5,−1,−1 | 3 | 1 | – | 0.3 |
| 6 | 11,5,−1,−1 | 3 | 1 | state 13 (doesn't exist in Update) | 0.3 |
| **7** | 13,13,13,13 | 2 | 1 | **10, 11** player defeated | 3.3 |
| **8** | 14,5,−1,−1 | 1 | 1 | **9** hit; also the hit timer | **2.1** |
| **9** | 18,−1,−1,−1 | 1 | 1 | **12** defeated (once, last frame held) | 4.6 |
| **10/11/12** | 15,5 / 16,5 / 17,5 | 3 | 1 | **idles**: wander action 0/3 ⇒ 10, 1/4 ⇒ 11, 2 ⇒ 12 (states 0, 1, 4) | 0.7 / 1.43 / 1.97 |
| 13, 14 | 15,5 / 17,5 | 3 | 1 | – | |
| **15** | 12,12,12,12 | 3 | 1 | **8** after landing | 0.43 |
| **16** | 16,17,15,16 | 1.5 | 1 | **5** hanging at the retreat point | 2.87 |
| 17 | 19×4 | 3 | 2 | – | |
| **18/19** | 20,21,21,21 / 21×4 | 3 | 2 | 19 = movement (like 2) | |
| 20 | 22,19 | 3 | 2 | – | |
| **21** | 23,24,24,24 | 3 | 2 | 6, 7 | |
| 22 / **23** | 25,19 / 25,19 | 3 | 2 | 23 = state 13 | |
| **24** | 27×4 | 2 | 2 | 10, 11 | |
| **25** | 28,19 | 1 | 2 | 9 hit | |
| **26** | 32 | 1 | 2 | 12 defeated | |
| **27/28/29** | 29,19 / 30,19 / 31,19 | 3 | 2 | idles (like 10/11/12) | |
| 30, 31 | 29,19 / 31,19 | 3 | 2 | – | |
| **32** | 25,26,26,26 | 3 | 2 | 8 | |
| **33** | 30,31,29,30 | 1.5 | 2 | 5 | |

Mode 1 thus only uses .ins anims **5..18**, mode 2 only **19..32**. The cinematic tracks 0/1 (root at y ≈ 5826 for W1B) and 2..4, 33..42
are **never** requested by the class; they are for scripts/cinematics. The "which movement is this" names above come from the usage, not from the
picture (uncertain).

### 4.2 Choice `vtbl[45]` = `0x410170` (tables `0x410574` per state, `0x4105ac` per wander action + 1)

```c
void Boss14_Anim(Boss14 *e) {
    int n = -1;  bool m1 = (e->mode == 1);
    switch (e->state) {
    case 0: case 1: case 4: {                                   /* 0x410190: action = Wander+0x50 (0x41c630) */
        int a = e->wander->action;
        if (a == 0 || a == 3) n = m1 ? 10 : 27;  else if (a == 1 || a == 4) n = m1 ? 11 : 28;  else if (a == 2) n = m1 ? 12 : 29;
        else if (a == 5 || a == 6 || a == 7 || a == 9 || a == 10) n = m1 ? 2 : 19;   /* a == -1 or 8: nothing */
        break; }
    case 2: case 3: if (e->chase->running /*+0x30*/ >= 0 && e->chase->running <= 1) n = m1 ? 2 : 19;  break;   /* 0x4102a5: always */
    case 5:  n = m1 ? 16 : 33;  break;         case 6: case 7:   n = m1 ? 4 : 21;  break;
    case 8:  n = m1 ? 15 : 32;  break;         case 9:           n = m1 ? 8 : 25;  break;
    case 10: case 11: n = m1 ? 7 : 24;  break; case 12:          n = m1 ? 9 : 26;  break;
    case 13: n = m1 ? 6 : 23;  break;
    }
    if (n >= 0) { e->anim->Request(n);  if (e->linkAnim) e->linkAnim->Request(n); }   /* same record number for both */
    e->anim->Tick(dt);  if (e->linkAnim) e->linkAnim->Tick(dt);
    if (e->state != 10 && e->state != 11 && e->state != 12) {                /* sound loop */
        SoundSrc_Active(&e->src);                                            /* 0x468e40 */
        SoundSrc_Play(&e->src, 0 /*2D*/, m1 ? 39 : 44, [0x5e48c8], -1.0f);   /* 0x468e50 */
    }
}
```
Because `vtbl[45]` runs in `Enemy::Update` before the switch, the animation matches the state of the **previous** frame. After death (mode 0) it no longer
ticks; the instance clock keeps playing anim 18 (record 9) to the end and holds the last frame. Whether record 9 actually starts before the freeze
depends on the priority/queue rules of `0x436a50` (ENEMY.md §8.6; equal priority) – presumably yes, not traced.

### 4.3 Duration `vtbl[53]` = `0x4105f0` (tables `0x410858`, `0x410890`; argument ignored)

Same layout as §4.2 but with `AnimLen(n, 0)`: 0/1/4 per wander action (idles 10/11/12 resp. 27/28/29, movement actions 2/19, action −1 or 8 ⇒ 0.0);
2/3 ⇒ AnimLen(2/19); 5 ⇒ 16/33; 6/7 ⇒ 4/21; 8 ⇒ 15/32; **9 ⇒ 8/25**; 10/11 ⇒ 7/24; 12 ⇒ 9/26; 13 ⇒ 6/23. Used by Wander (action duration:
a movement action thus lasts only AnimLen(2) = 0.167 s) and by **TakeDamage** (`hitT` and knockback time = AnimLen(8) = **2.1 s** in W1B).

## 5. Movement and height (answer to "where does he stand?")

* **Horizontal**: the normal behaviours (ENEMY.md §5): Wander (400 u/s, turning π/4 rad/s, leash 1000 around the home point), Chase (900 u/s,
  π rad/s), Stand. Subtype ≥ 9 ⇒ the shared movement function `0x41b2c0` keeps y fixed; `P+0x2c/0x30 = 15000` ⇒ no edge/step-up test, only
  the sweep against walls (§5.1). Plus the direct writes in state 6 (shaking) and 7 (1 % slide). **No TRAJ/path.**
* **Vertical** `vtbl[43]` = `0x410900`:
```c
void Boss14_Height(Boss14 *e) {
    float h2 = P->+0x28 * 0.5f;                                     /* 75 */
    vec3 p = e->pos + (0,h2,0);
    bool hit = Probe_Test(&e->probe, World_FindCell(&p), &p, h2, e->id);   /* 0x428ce0, 0x436dc0: press events */
    e->flags = hit ? e->flags | 1 : e->flags & ~1;   msgmask(e->id, 0x200) = hit;   /* 0x443e50 / 0x443e90 */
    if (e->flags & 4) {                                             /* 0x410b78: FALLING (state 7, 8, 10, 12) */
        float k = (e->mode == 2) ? 130.0f : 200.0f;                 /* feet sit k below pos */
        e->pos.y -= k;  Enemy_Ground(e);  e->pos.y += k;            /* 0x41a4e0 → 0x440330/0x440340: v += dt·g − 0.2·v per frame, g = the Fall
                                                                       object's +4 = P+0 at PostLoad = 200 (not Reset's 400/800) */
        return;
    }
    if (e->mode == 2 && !e->high && e->state != 9) {                /* BOBBING (mode 2 only, low phase) */
        float s = dt * P->+0x0c;                                    /* 400 */
        if (e->bobDown) { e->bob += s;  if (e->bob >= e->bobMax) {
                              Dust(&(vec3){pos.x, pos.y - 50, pos.z}, &(vec3){0,1,0}, 0, 1.5f, 6.0f);   /* 0x476140: smoke ring kind 0, PARTICLES.md 3 */
                              SoundFx(49, 0);  e->bob = e->bobMax;  e->bobDown = 0; } }                  /* 0x410a77 */
        else            { e->bob -= s;  if (e->bob <= 0) { e->bob = 0; e->bobDown = 1; } }
    } else e->bob = 0;
    if (e->high) e->home.y = e->yHigh;
    float d = e->home.y - e->pos.y - e->bob;
    if (fabsf(d) > 0.01f) {                                         /* 0x4a94f8 */
        float s = dt * P->+0x78;                                    /* 900 u/s */
        if (fabsf(d) > s) d = (d < 0) ? -s : s;
        e->pos.y += d;  Instance_SetCell(e, &(e->pos + (0,h2,0)));  /* 0x4077f0 */
    }
}
```
* **W1B hover heights** (mode 1): high = **2430** (placement), low = **1634.8** (floor 1344.8 + 290), landing: `pos.y` = floor + **200** (≈ 1544.8 if the
  arena floor there is also 1344.8). The low height is a **fixed** y (calculated below the start position), not ground-following.
* **The drop**: g = 200, v in units per frame (`v += dt·200 − 0.2·v`), so it reaches its terminal 1000 u/s within ≈ 0.1 s. W1B: 2430 → 1544.8
  in **0.97 s**; W2D (mode 2, feet 130 below pos): 3012 → 2336 (floor 2206) in **0.75 s** at 60 fps, 0.70 s at ~150 fps (port). With the
  400 / 800 the port used before, W2D's drop took ≈ 0.25 s.
* `pos` is the boss's logical point; his **cone** runs from `pos.y − 250` to `pos.y` (§6.1), his base collision cylinder (`vtbl[24]` = `0x41ad80`)
  from `pos.y` to `pos.y + 150` with radius 240. The model is drawn at `pos` with the rotation from H (`vtbl[44]`); the root node comes from the
  animation. If a port stays stuck on **anim 0** (cinematic track, root y ≈ 5826), the model hovers far above the arena: in the original, from the
  first Update in mode 1 onward, the class only ever plays records 2..16 (anims 5..18). Before the command (mode 0), 405 is hidden by the script.

### 5.1 The sweep `0x437580`: a SPHERE of radius 240 (the lanterns of W1B)

`0x41b2c0` calls `0x437580(&res, &from, &to, up, 30.0)` with `up = min(P+0x30, h/2) = 75` and `[0x4b3118] = P+4 = 240` (`0x41b489`):

```c
void Sweep(vec3 *res, vec3 *from, vec3 *to, float up, float sub /*30*/) {      /* 0x437580 */
    float h = r + up + 1.0f;                                    /* 240 + 75 + 1 = 316: sphere centre above pos */
    vec3 c = *from + (0,h,0), d = *to - *from;
    int n = (int)(floor(|d| / sub) + 1.0f + 0.5f);  d /= n;     /* substeps <= 30; at |d| = 0 exactly one */
    for (; n > 0; n--) {
        c += d;
        SpherePush(&c, r, -1);                                  /* 0x407340: world (0x409ad0) + instances vt[9] = 0x433ff0 */
        if ([0x4c4bd0]) { c.x += push.x; c.z += push.z; }       /* the full push-out, x/z only */
        GetHeight(&c);  if (c.y - h < groundY) c.y = groundY + h;
        *res = c - (0,h,0);
    }
}
```
* `0x407340` = sphere against all world polygons of the affected cells (front face, `0.001 < d < r`, edge tests) and the static + dynamic
  instances via `vt[9]` = **`0x433ff0`**, which (like the cylinder test `vt[8]` `0x433140` and the floor test `vt[7]` `0x432480`) iterates the
  list **`S+0x58/0x5c` = the press nodes (flag 0x01)**, **not** the hull nodes (`S+0x38`). Positive maximum + negative minimum per axis.
* The sphere thus spans from `pos.y + 76` to `pos.y + 556`. **Low** (1634.8) it touches the tops of the four lanterns in the corners of the arena
  (W1B inst 235..238, model 6, press nodes up to y 1972); **high** (2430) it clears them, but touches the rock walls behind the lanterns.
  A player standing in a corner behind a lantern is thus never within the 150 (xz) that state 3 needs to shake/stomp:
  Buzz stays ≈ 200 away from him in state 3 (measured in the port: Woody (−7983, −7567), Buzz (−7855, 2430, −7717)). That is the "hiding
  behind the lanterns" trick from the original.
* The sweep runs **every frame**, even with step 0 (Stand, shaking, the fall in state 7): the sphere then pushes him out of place from whatever it touches.
* After the sweep (`0x41b4dd..0x41b69c`, decompiled round 30): the behaviour hook `[2]` (Chase `0x41bdf0`: movement < 0.01 ⇒ a new target angle from a
  point ±16 off in x and z, unstick by heading only - the position is not moved); subtype ≥ 9 ⇒ `res.y = from.y`; `0x436d10` clears the probe's platform; `drop = res.y − [0x53a568]`. **Free** if
  `[0x4b310c] >= 0.8` (`0x4a987c`, NaN = blocked) and `drop < P+0x2c` (15000): then (with `drop < 1` and hit kind 2 the platform attach
  `0x436d80`) `pos = res`, collision centre `res + (0, h/2, 0)`, re-cell. Otherwise **blocked**: `pos += platform delta`, re-attach if
  the probe had a platform, re-cell, and the behaviour's `vtbl[3]` OnBlocked (Wander `0x41c420`: widest free sensor direction, action 5;
  Chase `0x41be90`: nearest free direction, turn timer 0). `[0x53a568]` / `[0x4b310c]` are the ground height and the normal y of the
  LAST substep's GetHeight `0x435650(&c, −1, 1)` at the sphere centre; when GetHeight finds nothing it answers `y = c.y`, normal
  (0, 1, 0) (`0x43568c`), so the sweep then lifts the sphere by h (`c.y − h < c.y`) and the step is free. Over the W1B arena floor
  the test always passes; it matters on slopes steeper than ~37° under the sphere (mode 2 arenas). Port: `enemy_sweep` / `enemy_common_move` (enemy.c, shared with every enemy since round 33, ENEMY.md §5.1) return both,
  `behav_move` does the test and the two OnBlocked (`player_ground_query_n`, the ground query with the normal). Log `WOODY_BOSSLOG=1`
  prints `boss blocked at ...`.
* The push-out per polygon (`r − distance` along the direction from the face or from every edge the sphere cuts, per-axis
  maximum/minimum; instances x/z only) is decoded instruction by instruction in PERSO_MOVE.md §6.8 (`0x409ad0` / `0x439d60`).

## 6. Damage

### 6.1 Touch `vtbl[31](cat, subtype)` = `0x410cf0` – cone pointing down

```c
Actor *Boss14_Touch(Boss14 *e, int cat, int sub) {
    for (i = 0; i < [0x4c5324]; i++) { Actor *a = [0x4c5258][i];
        if (a == e || (cat && Cat(a) != cat) || (sub && Subtype(a) != sub)) continue;   /* 0x40c340 / 0x40c350 */
        vec3 A = *a->vtbl[34](), S = *e->vtbl[34]();
        float bot = S.y - P->+0x7c;                                 /* pos.y − 250 */
        if (A.y + a->vtbl[33]() < bot) continue;                     /* actor entirely below the cone */
        if (S.y < A.y) continue;                                     /* feet above pos */
        float o = A.y - bot + a->vtbl[33]();                        /* how far the actor sticks into the cone */
        float r = a->vtbl[32]() + (o < P->+0x7c ? o * P->+0x80 / P->+0x7c : P->+0x80);   /* radius_a + 150·min(o,250)/250 */
        if ((A.x-S.x)*(A.x-S.x) + (A.z-S.z)*(A.z-S.z) <= r*r) return a;
    }
    return NULL;
}
```
Only the class itself calls this (state 3 low and state 7) with `(1, 0)` = the player. A hit ⇒ `Perso->vtbl[39](boss, 1.0, dir_xz, pos + dir·150 − (0,125,0), 0)`
(PERSO_MOVE.md §4.4: 1 heart, knockback, invulnerable). Outside those two states, touching does **no** damage.

### 6.2 TakeDamage `vtbl[39]` = `0x40fe90`

```c
bool Boss14_TakeDamage(Boss14 *e, Actor *att, float dmg, vec3 *dir, vec3 *pt, int kind) {
    if (e->high || e->hitT > 0) return false;                       /* high phase or still "hit": invulnerable */
    e->state = 9;
    e->hitT = e->vtbl[53](0);                                       /* AnimLen(8) = 2.1 s (W1B) */
    SoundFx(e->mode == 1 ? 42 : 47, 0);                             /* 0x40fef1 */
    if (e->mode == 1) {
        switch ((int)e->hp) {                                       /* hp BEFORE the hit (fistp to [0x4c5334]); table 0x410054 */
        case 1: smokeOn[0] = smokeOn[1] = smokeOn[2] = 0;  break;   /* last hit: smoke off */
        case 2: Explode(link, 0); break;   case 3: Explode(link, 1); break;   case 4: Explode(link, 2); break;
        default: break;                                             /* hp 5 (first hit): nothing */
        }
    } else {
        vec3 v[2] = { e->pos + (0,400,0), e->pos + (0,500,0) };     /* 0x4a964c, 0x4a9998 */
        Effect_Explosion(1, v, 0);                                  /* 0x477060 */
    }
    return Enemy_TakeDamage(e, att, 1.0f, dir, pt, kind);           /* 0x41adc0: damage ALWAYS 1.0 */
}
void Explode(Inst *link, int n) {                                   /* 0x40ff48 / 0x40ff7e / 0x40ffb1 */
    vec3 v[2];  GetVector(link, /*typecode*/0, v, n);               /* 0x42f6b0 */
    Effect_Explosion(1, v, 0);  Smoke_Attach(link, n);              /* 0x477060, 0x475f30 (§9.2) */
}
```
* **Who can hit him**: any `vtbl[39]` call (peck, air attack, `vtbl[40]` Blast), but only in the **low phase** (`+0x224 == 0`: from the
  landing until he ascends again) and outside the 2.1 s of "hit". Damage is always **1**: 5 hits. The linked instance 404 (type 17) has
  no `vtbl[39]`; hitting it does nothing.
* `Enemy_TakeDamage` (ENEMY.md §6.2): `Behav_Knock` on the **active** behaviour (knockback `600·t_rest` along `dir` for AnimLen(8) = 2.1 s;
  at `dir = 0` (peck) no displacement); if that behaviour already has a knockback in progress ⇒ `false` **without** hp loss (state 9 and hitT are
  already set at that point – an edge case, literally like this). Star burst `0x40c2d0` at `pt` (except `kind == 2`; `0x41adea`;
  `0x40c2d0` skips it for `pt == NULL` and calls the hit star `0x4750e0(pt)` otherwise, `0x40c2ff`/`0x40c327`), then `hp −= dmg`
  (`0x41adef`). Return value `hp ≤ 0`. A bomb blast reaches it through the base `vtbl[40]` `0x41ae20` = `vtbl[39](0, hp, &dir, NULL, 0)`:
  no star. (Port round 30: `boss_take_damage(e, pt, kind)` draws `game_hit_star` after the knockback check.)
* Whether the player can peck him from the floor or has to jump depends on the Perso's attack geometry (PERSO_JUMP.md) relative to `pos.y` ≈
  floor + 290 and has not been statically determined.
* After the hit: state 9 (red/white flashing §8, anim 14), after 2.1 s `high = 1` ⇒ up. On the 5th hit: state 9 → 12 (next frame).

### 6.3 Death / end (state 12)

`SetVar(mailVar, 3)`, stop the sound loop, `mode = 0`. **No** `vtbl[57]`, no `+0x10c` removal, no fade (`+0x15c` doesn't advance),
type word bit 0x400 stays set. Because Update stops immediately in mode 0, he also stops falling (flag 4 has no effect anymore): he freezes in the air
and the HUD bar stays with 0 dots (nothing sets `hud+0x48` back to 0; only the HUD init `0x4471c6`).

## 7. Messages `vtbl[22]` = `0x410070`

| id | args | code | effect |
|---|---|---|---|
| **59** | inst, a | `0x4100d2` | `link (+0x234) = World->inst[a & 0xffffff]` (`[0x50944c]+0x6c`). If `link && link+0x108 (u8) ≠ 0` (type 17: "reset"): `linkAnim (+0x1c8) = new AnimCtrl14(link)` + `vtbl[4]()` reset; `link->flags8 |= 0x20` (INSTANCE.md: no re-cell at the animated position); `0x40c5a0(link, this)` ⇒ `link+0x110 = boss` (owner for the render colour). One instance, no list |
| **60** | inst, var | `0x4100b0` | `mailVar (+0x230) = var & 0xffffff` |
| other | | `0x41a740` | `Enemy::HandleMsg` (6 on/off, 11 parameters, 11/4 = Reset) → Instance/FadeInst |

Always returns 0. **Correction to MESSAGES.md** ("59/60 in classes 14-16 link 8 instances to `+0x1c8..+0x1e4` and set flag 0x40"): that is the
handler of class **15** (`0x40e781..0x40e7d8`: 8 instances in `+0x1c8..+0x1e4`, OR into `inst+8`, then Reset; message 60 ⇒ `+0x24c` at `0x40e7e3`).
Class 14 links **one** instance to `+0x234`, sets flag **0x20**, and creates an AnimCtrl for it.

## 8. Render colour `vtbl[26]` = `0x40fde0` (also for the linked instance)

```c
void Boss14_RenderColour(Boss14 *e) {
    if (e->state == 9 || e->state == 12) {
        if (e->hitT < 0) e->hitT = 0;
        g_colMode /*[0x5ac850]*/ = 1;  g_col.r /*[0x5ac854]*/ = 1.0f;
        int n = ++[0x4c5348];
        g_col.g = g_col.b /*[0x5ac858], [0x5ac85c]*/ = (n <= 8) ? 0.0f : 1.0f;
        if (n == 16) [0x4c5348] = 0;
        [0x5ac860] = 0;
    } else { g_colMode = 0; [0x5ac860] = 0; }
}
```
Colour mode 1 = multiply (MENU_LOAD.md, LIGHTING.md) ⇒ alternating **red** (1,0,0) and normal (1,1,1), 8 draw calls each. The counter
is global and is incremented per model drawn (boss + 404 ⇒ roughly 4 frames red, 4 frames normal). Type 17 calls the owner via `vtbl[26]` = `0x40c5c0`,
so 404 flashes along with him.

## 9. The linked instance (W1B 404, type 17) and effects

### 9.1 Drag-along `vtbl[55]` = `0x41b1b0` (base, active because `vtbl[54]` returns the link)

```c
void Enemy_SyncLinked(Enemy *e) {
    Inst *l = e->vtbl[54]();  if (!l) return;
    l->colCenter = e->pos + (0, P->+0x28*0.5f, 0);   /* +0x60..0x68 */
    l->pos = e->pos;  Instance_SetCell(l, &(e->pos + (0,75,0)));   /* 0x4077f0 */
    memcpy(&l->rot /*+0x28*/, &e->rot, 9*4);                         /* rotation matrix */
}
```
Every frame (via `Enemy::Update`, so only in mode ≠ 0) 404 sits at **exactly the same** position and rotation as 405 and plays via `+0x1c8` the same
logical record with **its own** .ins animations (model 38 has 19 anims ⇒ sub 5..18 of mode 1 exist). Because 404 has its own AnimCtrl but
`AnimLen` always comes from 405, the two models run in parallel. 405 = Buzz, 404 = his craft/vehicle (presumably; the cone collision matches a
craft under Buzz – uncertain).

### 9.2 Class 17 (brief; only what the boss needs)

Ctor `0x40c3d0` (Instance base `0x42e1a0`; `+0x104 = 0`, `+0x108 = 0`, `+0x10c = 0` smoke emitter, `+0x110 = 0` owner, `+0x114 = 1`), vtable `0x4a95dc`
(28 slots, Instance level: **no** `vtbl[39]`): `[1]` PostLoad `0x40c440` (`0x44e7c0`, `+0x110 = 0`, `+0x108 = 0`), `[3]` Think `0x44e810` (FadeInst),
`[17]` Reset `0x40c460` (`0x42e250`; **`+0x110 = 0`**, `+0x108 = 1`; only if `+0x114 == 0`: once, a smoke emitter over its markers with typecode 9
(list `0x5e8564`)), `[22]` `0x40c5e0`: message **63** `[inst, v]` ⇒ `+0x114 = v`, Reset; rest → `0x44e8f0`. `[26]` `0x40c5c0`: owner ? `owner->vtbl[26]()` :
`0x430010`. W1B doesn't send 63 ⇒ `+0x114 = 1` ⇒ no emitter ⇒ the line `link->smoke->on = 1` in the Update prologue does nothing in W1B. Order in W1B
is correct: 1200 (PostLoad + Reset ⇒ `+0x108 = 1`) before message 59 (0.05 s later); a later Reset of 404 would clear the owner.

**Smoke plume** `0x475f30(inst, n)` (on hits with hp 4/3/2 before the hit ⇒ marker n = 2/1/0): particle emitter (pool `[0x5e823c]+0xdb8`, max 2000,
lifetime 100000 s, callback `0x475d90`) at typecode-0 marker no. n of 404, sets `smokeOn[n] = [0x5e857c + n] = 1`. As long as that byte is 1: **300 particles/s**
(`0x4a986c`) along the marker's path, ±15 jitter in x/z, particle `0x475cd0`: 0.5 s, sprite `0x1000e`, white, alpha 0.5·(1 − u), rises 50·u
(u = t/0.5), size `rand·10 + 50u + 20`, flags 7 (exact decompile: PARTICLES.md §10). The last
hit (hp 1) and Reset set the three bytes to 0 ⇒ the emitters die out. Explosion `0x477060(1, v, 0)` at the same marker (same effect as script message 1509
mode 4/5).

## 10. Sound, camera, HUD, events

| what | id (ref) mode 1 / mode 2 | address | when |
|---|---|---|---|
| loop (2D, source `+0x248`) | **39** (76) / **44** (81) | `0x410553` / `0x410569` | every Update in states ≠ 10/11/12; stopped in 10 (`0x40fcf3`) and 12 (`0x40fc9d`) |
| landing stomp | **40** (77) / 45 (82) | `0x40f9e5` | state 7, landed |
| player hit | **41** (78) / 46 (83) | `0x40f291`, `0x40fb78` | cone contact (state 3 low, 7) |
| boss hit | **42** (80) / 47 (85) | `0x40fef1` | TakeDamage accepted |
| player defeated | **43** (79) / 48 (84) | `0x40fcc6` / `0x40fcde` | state 10 |
| bob bottom | – / **49** (12, vol 100) | `0x410a77` | mode 2, low phase |

All SoundFx with `inst = 0` (2D). Camera shake `0x41fbb0(cam, 1.5 s)` on landing (`0x40f946`) and on contact during the fall (`0x40fb88`).
HUD: `0x4484d0(1, hp, 5)` every Update (mode ≠ 0) ⇒ the boss bar slides in on the first call (HUD_TEXT.md §4.4). Events: msgmask **0x200** (on the
ground, `0x410993`/`0x4109ab`), 0x10 cleared in `Enemy::Reset`; **script var**: `mailVar := −1` (every Update), `:= 3` (defeated).

## 11. The W1B script around the boss (for checking the port)

* **Object 405** (boss): init `var51 = −1; var49 = 0; var51 = 3`; `1200 [405, 14]`, `45 [405, 33]`; after 0.05 s `59 [405, 404]`, `60 [405, var51]`;
  after 1 s `6 [405, 0]` (hide). Watchers:
  * `var0 == 1 && var49 == 1` ⇒ `var0 = 0; var51 = 3; var49 = 0;` after 0.1 s `var49 = 1`. **var 0** is written by **object 8 = the Perso**
    (`1200 [8, 1]`): init `var0 = 0`, and `MSGTEST 16` (msgmask 0x10 = "Woody lost a life", pulse from `0x44c730`) ⇒ `var0 = 1`. It is thus a
    latch "Woody has died at least once"; the branch only fires at the moment the fight starts (var49 is only 1 for one tick) and delays the start by 0.1 s.
    `var51 = 3` is not a command to the boss (he replies −1). It is not the boss that writes var 0 (except the −1 edge case of §3.4).
  * `var49 == 1` ⇒ `var49 = 2`, `6 [405, 1]`, `6 [404, 1]`, **`var51 = 1`** (⇒ mode 1 + Reset on the next Update), 1152/1150, after 0.5 s rail camera 403
    (580 / 540 / 710).
  * `var51 == 3 && var49 == 2` ⇒ `var49 = 0; var46 = 4`; after 0.5 s `6 [405, 0]`, `6 [404, 0]` ⇒ object 397 plays cinematic 73 (1131) with 398/399,
    explosions `1509 [405, 399, 4/5, …]` and finally **1083** (EndLevel).
* During the fight, the boss is **not** reset when Woody dies (the Perso reset `0x445b23` only touches the Perso); var49 stays 2.
* W2D (745 + 746, `63 [746, 1]`, var 288), W3D (775 + 776, var 318) and WWS (362 + 363, var 138) write **2** ⇒ mode 2.

### 11.1 W2D: the reset after a death in the arena

Unlike W1B, W2D puts Buzz back when Woody loses a life (W3D the same, BOSS15_16.md §9.3):

1. The Perso's death: `0x44c730` pulses msgmask 0x10 on the Perso's object (port: `player.c`, game state 4); W2D object **3** (the Perso,
   `MSGTEST 16`) sets **var0 = 1**. (The engine itself resets no actor: `0x445930 → 0x40c040` calls `vtbl[28]` = `0x445840` = `ret`.)
2. Object **739** (the fight director) watches var0: `var0 == 1 && var291 == 1` (var291 = 1 since Woody entered volume 336, object 748,
   which also started phase 1) ⇒ `var292 = var293 = var294 = 1`, `var288 = var289 = −1` (both mailboxes), `var286 = 2903`, `var285 = 2`,
   `var0 = 0` (any other wake also clears var0).
3. Object **745** watches var292: `11 [745, 4, 0]` (Reset §3.3: start position and angle, hp 5, state 0, high, mode kept), `6 [745, 0]`,
   `6 [746, 0]`, `var292 = 0`. Object 762 does the same for class 15 (`11 [762, 4, 0]`, Boss15_Reset: hp 6, phase 0).
4. Phase 2 again with the intro skipped (`2903 − var286 − 50 < 0`): fade `1152`, after 0.7 s `6 [745, 1]`, `6 [746, 1]`, **`var288 = 2`**.
   The mode is still 2, so the mailbox only acknowledges (no second Reset); the fight restarts from the start position with 5 hp.

Port (verified at 60 fps and uncapped, `WOODY_BOSSLOG=1`): `BOSS 745 reset: pos −3849 3012 14286 ang 1.571 hp 5 mode 2` ≈ 3 s
after every death (also after hits: hp 3 → 5), and the boss reappears there 0.6 s later. Before, `boss_reset` put pos and home on the
instance position (= where `enemy_place` had last put him) and kept his angle, so he resumed the fight from wherever he was.

## 12. Recipe for the port (W1B playable and correctly on screen)

Order of implementation; numbers for mode 1 / W1B.

1. **Message 59 and 60** on type 14: `link = inst[a]`, `mail_var = var & 0xffffff`; link: `no_recell = 1`, own animation state, owner = boss.
   Type 17 (404) gets no other behaviour: visibility via message 6 like any instance.
2. **Mailbox** at the start of every boss update: `v = var[mail_var]`; `v == 1/2` ⇒ if the mode changes: `mode = v; boss_reset()`; always `var[mail_var] = -1`
   (wake watchers). `mode == 0` ⇒ do nothing else (no animation choice, no HUD, no drag-along either).
3. **Activation** as with `Enemy::Think`: only update if the boss is within **3000** of the camera (or hp ≤ 0) and no cinematic is running.
4. **Reset** (`boss_reset`): pos = home = **start position** (the placement, −7031, 2430, −8863; not the current position), H angle = start angle,
   hp = 5, `high = 1`, state 0, falling off, make visible, smoke off, `walk = 400, run = 900` (P+0 = 400 is written but unread: the fall keeps g = 200);
   `y_high = 2430`, `y_low = floor_below_start + 290` (**1634.8**, determined once at load time).
5. **Animation**: never leave anim 0 in place. Table logical record → (.ins anim, speed, chain): 2 → (7, 3, loop), 4 → (9, 3, then 10 looped), 7 → (13, 2, loop),
   8 → (14, 1, then 5), 9 → (18, 1, hold), 10/11/12 → (15/16/17, 3, then 5), 15 → (12, 3, loop), 16 → (16,17,15,16, 1.5). Choice per state §4.2
   (from the previous frame). **Same record on 404** with its own animations. `AnimLen(8) = len(14)/1 = 2.1 s`, `AnimLen(2) = 0.167 s`.
6. **Movement**: horizontal the existing wander/chase from `src/enemy.c` (but: keep y fixed, no edge/step-up test, turning π/4 resp. π, speeds 400/900,
   leash 1000 in 3D); vertical §5: towards `home.y` at max 900 u/s, or falling with **`g = 200`** (the Fall object's, per-frame formula of ENEMY.md §3.3)
   with the feet 200 below `pos` (mode 2: 130). Every wander / chase start zeroes that behaviour's knock timer (§3A.1).
7. **Drag-along**: after the boss update, 404.pos = 405.pos and 404.rot = 405.rot (including the cell).
8. **State machine** §3A literally (13 states, including the 60 Hz shaking −20/+20/+20/−20 + 10 % towards the player, the 1 %/frame slide during the fall,
   the retreat 1300 from the player while he stands still (< 1 unit/frame), and the 20-unit gate before the stomp).
9. **Cone contact** §6.1 (height 250 below `pos`, radius `r_player + 150·min(o,250)/250`) ⇒ `player_hit(1 heart, dir_xz, pos + dir·150 − (0,125,0))`;
   player dead ⇒ state 10 (4 s cheering, SoundFx 43, stop the loop).
10. **Vulnerability**: `enemy_take_damage` for type 14: only if `!high && hit_t <= 0`; always 1 hp; `state = 9; hit_t = 2.1`; SoundFx 42; at hp-before 4/3/2:
    explosion + smoke plume at typecode-0 marker no. 2/1/0 of 404, at hp-before 1 all smoke off. Red/white flashing in states 9 and 12.
11. **End**: state 12 ⇒ `var[mail_var] = 3` (**before** the VM tick of the same frame, so object 405 sees it), stop the loop, `mode = 0` (freeze).
12. **HUD**: every active frame `hud_boss_bar(1, (int)hp, 5)` (bar with Buzz's face, HUD_TEXT.md §4.4).
13. **Sound/camera**: loop 39 (2D) in all active states except 10..12; 40 landing + camera shake 1.5 s; 41 player hit (+ shake only during the fall); 42 hit; 43 player defeated.

## 13. State of the port (`src/boss.c`)

* Type 14 is a regular `Enemy` in `g_enemies` (so peck, air attack and auto-aim find him) with its own fields in `Enemy.b`; `enemy.c` forwards
  Update, TakeDamage and `11/4` to `boss.c`. Engine hooks in `main_engine.c`: `game_var_get/set` (mailbox), `game_cam_shake` (`0x41fbd0`,
  now in `cam_update`), `game_boss_bar` (`hud_boss_bar`), `game_explosion` (two flash records), `game_boss_smoke` (explosion + smoke plume at
  typecode-0 marker no. n of the saucer).
* Movement: `boss_sweep` = the sphere sweep §5.1 with `player_sphere_push` (world + press nodes of instances, excluding the boss himself, his
  saucer and the player), every frame, even at step 0. Before, it was a thin ray only against world polygons and nothing was tested at step 0:
  Buzz flew straight through the lanterns and reached the player in the corners. The "free" test after the sweep (§5.1) and the
  hit star (§6.2) are ported (round 30); the smoke plume's colour and alpha are the decompiled ones (PARTICLES.md §10).
* Hit tests (round 30): the peck dash and the charge run use the exact `0x433920` / `0x433de0` (PERSO_JUMP.md §3.1). For Buzz
  (R 240, H 150) the dash capsule is two spheres of 340 (100 + 240) around `pos.y − 90` and `pos.y + 240`, so an air attack within
  ≈ 340 of him hits; the charge run's range is `pos.y − 75 + 0.1 .. pos.y + 225`, above the beak (≈ feet + 124 = 1469 in W1B) at the
  low hover height 1634.8 and only 1 unit short of it at the landing height 1544.8: from the ground Buzz can only be hit by jumping.
* Mode 2 checked against §3A/§5/§6.2 (round 30): Reset values, bob (P+0x0c, 150, sound 49, dust), fall offset 130, records +17,
  sounds 44..49, the explosion at `pos + 400` on hits; class-17 smoke never exists (W2D/W3D send `63 [inst, 1]`, §9.2). Nothing
  mode-2-specific is missing beyond the two items above.
  Test corner: `WOODY_POSAT="24 -7990 1360 -7560"` for the test below (Woody is no longer hit; the old build kills him).
* Reset, drop and knock timers (W2D reports): Reset puts pos / home / angle on the start (`Enemy.start` / `start_ang`, §3.3) and the instance with it; the drop uses the Fall
  object's g = 200 (`BossState.fall_g`, §5); wander / chase starts zero their knock timers (§3A.1: no more shaking after a hit, every hit
  costs hp); the 1 % slide of state 7 is normalised to 60 Hz. W2D test: `--pos -3845 2300 16300 --yaw 180 --walk 1.5` (fight from ≈ 28 s),
  `WOODY_BOSSHIT=1` (testing: a 1-hp peck as soon as he is low and within 400), `WOODY_GOD=1`, `WOODY_KILLAT="44 3"`; the boss log
  now also prints the three knock timers and the game time.
* The linked instance is excluded from the boss's ground test by `player_set_carried`; `player_ground_query` now also skips the player himself
  (the boss used to land on Woody's own collision node).
* Found while porting: the cinematic start `0x44ecc0` calls `0x4077f0` on every actor, and that always puts a hidden instance back into its
  cell. Because of that, Buzz (398) and his saucer (399) appear in the intro of the fight, even though the script hides them at init (CINEMATIC.md).
* Test: `extract/Data W1B --pos -5753 1800 -8901 --yaw -90 --walk 2` (walks into volume 94; intro until ≈ 22 s, then the fight);
  `WOODY_FPS=60 WOODY_POSAT="25.5 -5912 1345 -8491" --jump 25.55 --peck 25.85 0.1` puts Woody ≈ 330 from him after the
  first stomp and hits him (hp 4) with the air attack during the low chase (round 30; the older `--jump 25.2 --peck 25.5 0.1` no
  longer lands a hit, neither in the round-29 build). Hooks: `WOODY_BOSSLOG=1` (state per frame), `WOODY_BOSSHP=N` (start hp),
  `WOODY_FPS=N` (frame cap, for the per-frame formulas), `WOODY_GOD=1`. A run to the end completes W1B in `woodyre.sav`: make a copy first.

### 13.1 WWS: the final fight never started (fixed)

After the fight intro in WWS Buzz vanished and only the wooden arena fence was left (every release: the scripts of 1.00
and the Feb-2002 build are the same apart from three empty objects). Cause: the port
celled an enemy whose mover had not run yet on its ANIMATED root. Buzz 362 sits on anim 0 (cinematic track), root
2435 853 -4846: no floor group, so he was never in the instance list, Think never called Update and the mailbox (var 138)
never saw the script's 2. The original never re-cells a flag-0x20 actor by its clock, so it keeps the loader's cell at its
.ins position (1519 761 -6811, group 8) until its first Update. Fix: `cell_point` (render_gl.c) uses `position` for types
4..16 until the mover sets `cell_dy`. W1B / W2D / W3D / W2B / W1A logs are identical before and after. Test (EN or PL data):
`WOODY_KEYS="3:RET 5:RET 7:RET 9:RET 11:RET 13:RET 17:RET 20:DOWN 21:RET 24:RET 27:RET" out/woody.exe <Data> WWS --prev W3D
--alldone --nologo` (run it from a scratch folder: the keys answer the save prompt; WOODY_KEYS needs the wall clock, not
WOODY_FIXDT, as the menu stops the game clock). Fight at ~41 s; `WOODY_BOSSHP=1 WOODY_GOD=1 WOODY_BOSSHIT=1` wins it:
`BOSS 362 beaten: var 138 := 3`, cinematic 73, 1180, Credits.

## 14. Open questions

* Visual meaning of anims 5..18 (model 37) and 5..18 of model 38; which ones exactly are "flying", "laughing", "hit" is derived from usage.
* Slot 23 (`0x45b340` = 8) and `P+0x84`/`P+0xa0` (2.0): no reader found. `+0x1d0` (still time) and `+0x22c` have no reader.
* The comparison in state 5 (`arc(target, angle > 0 ? 1 : 0)`) is taken literally as is; presumably a bug in the original.
* Whether the camera stays within 3000 of the boss throughout the fight (otherwise he freezes and his sound stops) and whether the player can
  hit him from the floor is not statically determined; tracing the original (`tools/wtrace.py`) at `0x40fe90` and `0x40eec0` could settle that.
* The ground height 1344.8 was determined only over world polygons; `0x435650` may also take instance hulls into account.
