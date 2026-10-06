# INPUT.md — Woody.cfg key bindings, joystick, the action layer and the pause menu restart (Woody.exe, build 17-10-2001)

Status: **static analysis** of `out/disasm_full.txt` (imagebase 0x400000) and of `Setup.dll` (imagebase 0x10000000,
disassembled with capstone); only the mouse check of §1.3 was traced live. Addresses without a module are Woody.exe. Uncertain points are
marked **uncertain**. Related: PERSO_MOVE.md §3 (the 14 actions and who reads them), MENU_NEWGAME.md §1 (menu keys),
TRACING.md §2 (Woody.cfg and `tools/native/mkcfg.c`), RACE.md §3.2 (race restart).

## 0. Summary

- The game never reads a key directly. Every frame `0x402940` turns the keyboard and the joystick into **14 actions** of the
  controller `[0x5e6188]` (`app+0x14`): value, hold time, state (PERSO_MOVE §3.2). Perso, camera and menus only read actions.
- **Woody.cfg** is read once at boot (`0x401000`): magic `0x19072001` + 0x11c bytes copied 1:1 to `0x4c2bd0`. The key part
  is **two sets of 12 codes** (cfg+0xac "config 1", cfg+0xdc "config 2"); a code < 0x200 is a DirectInput scan code (DIK),
  0x200 + n is joystick button n. There is no in-game key menu: only `Detect.exe` / `Setup.dll` write the file.
- The app ctor `0x44fbd0` copies the codes into a table of **12 actions × 2 bindings** (`app+0x108`) and sets the
  **input mode** `app+0x104`: 0 = keyboard only (cfg+0x114 == 1), else 1 or 2 (joystick; the two behave identically in Woody.exe).
- Joystick = the **first attached DirectInput joystick** (`EnumDevices(DIDEVTYPE_JOYSTICK, ATTACHEDONLY)`, `0x4678d0`), data format
  `c_dfDIJoystick`, exclusive + background, **X/Y range ±4096, dead zone 30 %**, the rest scaled to 0..1 (`0x467a80`).
  X/Y give actions 0/1 and 2/3 **with the deflection as value**; the speed of the Mover scales with it (`0x45a4b0`).
  Only X, Y and buttons 0..31 are read: no POV hat, no second stick, no rumble (`0x467b20` = `ret 8`).
- In the joystick modes the **keyboard gives no directions** (`0x40301e`); all other actions work from both.
- The **mouse** is a DirectInput device (`GUID_SysMouse`, non-exclusive + foreground, `0x467b70`) that is created and acquired
  but **never polled** (`0x467cc0` has no caller): its `lX`/`lY` stay 0, and the look-around that reads them (`0x459346`) only
  turns with the keys (§1.3).
- Menus: confirm = action 0xc (set together with action 4, the jump key) just pressed **or DIK_RETURN released** (fixed);
  back = action 5 just pressed **or DIK_ESCAPE released** (fixed). Pause = action 9 just pressed, only in Game state 2.
- Pause page **0x19** is the pause menu **while riding** (Perso state 1): Continue / **Start again** / Options / Quit.
  "Start again" (result 18, `0x40584d`) = respawn (`0x445930`) + race restart (`0x4560f0`): back to the level start, no
  checkpoint, iris shut for 0.1 s then opening over 1 s, no life lost. Outside a race there is no restart item.

## 1. Objects

| object | global | ctor / vtable | notes |
|---|---|---|---|
| DirectInput | `[0x5e619c]` | `0x4676f0`: `DirectInputCreateA(hinst, 0x700, &di, 0)` → `[0x4b6f80]` = HRESULT (≠ 0: no input objects at all) | refcount `[0x5e61a0]` (`0x467720` / `0x467730`) |
| keyboard | `[0x5e6194]` = `app+8` | `0x467680` → `0x467d70`, vtable `0x4ab974`, 0x308 B | `GUID_SysKeyboard` (`0x4abf10`), `c_dfDIKeyboard` (`0x492770`), coop 6 = non-exclusive + foreground |
| mouse | `[0x5e6190]` = `app+0xc` | `0x4674f0` → `0x467b70`, vtable `0x4ab944`, 0x18 B | `GUID_SysMouse`, `c_dfDIMouse`, coop 6 = non-exclusive + foreground; read only by the look-around camera (`0x459346`), and **never polled** (§1.3) |
| joystick | `[0x5e618c]` = `app+0x10` | `0x467470` → `0x4678d0`, vtable `0x4ab90c`, 0x5c B | `+4` vibration (1.0 after the ctor, MENU_OPTIONS.md); `[0x5e618c] = 0` when no device was found |
| controller | `[0x5e6188]` = `app+0x14` | PERSO_MOVE §3.2 | 14 × {value, held, state} + dt |

All four are created in `0x402320` (app init) whatever the mode; the mode only decides what `0x402940` polls.

### 1.1 Keyboard `0x4ab974`
| vt | addr | |
|---|---|---|
| 1 | `0x467ef0` | poll: `GetDeviceState(256)`; each DIK byte ≠ 0 → `state[table_4b6f88[dik]] = 1`; lost / not acquired → `Acquire` |
| 2 | `0x467f70` | **code → internal code**: `code > 0x100 ? 0x90 : table_4b6f88[code]` |
| 3 | `0x4675a0` | frame: `prev = cur`, poll, `released[i] = prev[i] && !cur[i]` |
| 4 | `0x4675f0` | held (internal code 0..0xff, else false) |
| 5 | `0x467610` | just released |
| 6 | `0x467640` | first released code or 0x90 |

`table_4b6f88` renumbers the DIK codes densely: DIK 1..0x53 → 0..0x52, then 0x56 → 0x53, 0x57 → 0x54, …, extended keys
(0x9c Num Enter → 0x69, 0x9d RCtrl → 0x6a, 0xc8 ↑ → 0x78, 0xcb ← → 0x7a, 0xcd → → 0x7b, 0xd0 ↓ → 0x7d, …) up to
0xed → 0x8f; everything else → **0x90 = no key**. The fixed menu keys use internal codes: 0 = DIK_ESCAPE, 0x1b = DIK_RETURN.

### 1.2 Joystick `0x4ab90c`
```c
Joystick::Joystick() {                                   /* 0x4678d0 */
    if (DI failed) return;
    g_joy = this;  dev = NULL;
    di->EnumDevices(4 /*DIDEVTYPE_JOYSTICK*/, cb_467760, &dev, 1 /*DIEDFL_ATTACHEDONLY*/);
    if (!dev) { g_joy = NULL; return; }                  /* 0x467943 */
    dev->Acquire();                                      /* 0x467a30 */
}
BOOL cb_467760(const DIDEVICEINSTANCE *d, void *ref) {   /* the FIRST attached joystick only */
    if (di->CreateDevice(d->guidInstance, &dev, 0)) return CONTINUE;
    if (dev->SetDataFormat(&c_dfDIJoystick) || dev->SetCooperativeLevel(hwnd, 9 /*EXCLUSIVE|BACKGROUND*/)) { dev->Release(); return CONTINUE; }
    DIPROPRANGE r = { {0x18, 0x10, 0 /*DIJOFS_X*/, 1 /*BYOFFSET*/}, -4096, 4096 };   /* 0x4b6f84 = 4096.0 */
    dev->SetProperty(DIPROP_RANGE, &r);  r.diph.dwObj = 4 /*DIJOFS_Y*/;  dev->SetProperty(DIPROP_RANGE, &r);
    g_joy->dev = dev;  return STOP;
}
void  vt1_poll()   { dev->Poll(); memset(&st, 0, 0x50); if (dev->GetDeviceState(0x50, &st) == DIERR_INPUTLOST) dev->Acquire(); }   /* 0x467a40 */
float vt3_x()      { return axis(st.lX); }             /* 0x467ad0 */
float vt4_y()      { return axis(st.lY); }             /* 0x467ae0, down = + */
bool  vt5_btn(n)   { return n >= 0 && n < 32 && st.rgbButtons[n] & 0x80; }            /* 0x467af0 */
void  vt2_rumble() { }                                 /* 0x467b20 */
bool  vt6_ok()     { return true; }                    /* 0x467b40: "controller connected" */
float axis(int v) {                                    /* 0x467a80 */
    int dz = ftol(4096.0f * 0.3f /*0x4aab98*/);        /* 1228 */
    if (v > 0) { v -= dz; if (v < 0) v = 0; } else { v += dz; if (v > 0) v = 0; }
    return (float)v / (4096.0f - dz);                  /* -1..1, 0 inside the dead zone */
}
```
`vt6` is always true on PC; the console path behind it (`0x403360`: controller gone for > 0x46 frames → page 0x1a,
handler `0x405896`: action 9 → back to page `app+0x64`) never runs.

### 1.3 Mouse `0x4ab944`
```c
Mouse::Mouse() {                                         /* 0x467b70; new(0x18) at 0x467506 */
    vtable = 0x4ab944;  0x467720();                     /* DirectInput refcount */
    if (DI failed) return;                               /* [0x4b6f80] != 0 */
    dev = NULL;
    if (di->CreateDevice(GUID_SysMouse /*0x4abf20*/, &dev, 0)) return;                   /* 0x467bc5 */
    if (dev->SetDataFormat(&c_dfDIMouse /*0x491750*/)) return;                          /* 0x467bd6 */
    if (dev->SetCooperativeLevel(hwnd /*[0x4c3a94]*/, 6 /*NONEXCLUSIVE|FOREGROUND*/)) return;   /* 0x467beb */
    dev->Acquire();  g_mouse /*[0x5e6190]*/ = this;     /* 0x467bf7, 0x467bfa */
}
void vt1_poll() { memset(&st /*+8, DIMOUSESTATE*/, 0, 16); if (dev->GetDeviceState(16, &st) == DIERR_INPUTLOST) dev->Acquire(); }   /* 0x467cc0 */
int  vt2_x()    { return st.lX; }                        /* 0x467d00 */
int  vt3_y()    { return st.lY; }                        /* 0x467d10 */
bool vt4_b0()   { return st.rgbButtons[0] != 0; }       /* 0x467d20 */
bool vt5_b1()   { return st.rgbButtons[1] != 0; }       /* 0x467d30 */
/* vt0 0x467c20: dtor 0x467c40 (g_mouse = 0, Unacquire, Release) */
```
`GetDeviceState` is only in `vt1` `0x467cc0`, and **nothing calls it**: `[0x5e6190]` is read at `0x4022fb` (the app dtor's
test), `0x459346` (vt[2]/vt[3] only) and in the dtors `0x467550`/`0x467c65`; `app+0xc` is written at `0x402346` and read
nowhere (no `call [reg+4]` in the exe has the mouse as `this`); the frame input `0x402940` polls the keyboard (`app+8`) and the
joystick (`app+0x10`) only. The ctor never writes `+8..+0x17` either, so `lX`/`lY` are whatever `new` returned. Live
(`tools/wmouse.py`, 100 s from boot through the logos to the title): `0x467cc0` is never hit, the object at `0x026f8330`
holds `lX = lY = lZ = 0`, buttons 0, at VM init and every 5 s after. So **the mouse does nothing in Woody.exe**: the look-around
reads 0 and only the direction keys turn the view (PERSO_LOOK.md §3.1). The DirectInput device is created and acquired anyway
(non-exclusive, so the cursor stays; foreground only).

## 2. Woody.cfg

`0x405d40` (WinMain part) → `0x401000("Woody.cfg")`: open (up to twice; a missing file starts `Detect.exe` first, then
"Cfg not found !!!"), read `u32`, must be `0x19072001` (else log "Configuration file is Obsolete..." and fail), then read
0x11c bytes to **cfg = `0x4c2bd0`**. Written back by `0x401130` at exit (`0x405ef0`, same layout). File offset = cfg offset + 4.
`Setup.dll` builds the same struct at `0x10021d88` (`SaveConfig` `0x10003990`).

| cfg | file | Woody global | value in the generated cfg | meaning | read by |
|---|---|---|---|---|---|
| 0x00, 0x04 | 0x04 | `0x4c2bd0/d4` | 1, 1 | Setup: display / 3D detection done | – |
| 0x08 | 0x0c | `0x4c2bd8` | 5 | selected 3D device index (Setup) | – |
| 0x0c | 0x10 | `0x4c2bdc` | `84e63de0-46aa-11cf-…` | **D3D device GUID** (IID_IDirect3DHALDevice) | `0x4027f5` (TRACING.md had 0x0c/0x1c swapped) |
| 0x1c | 0x20 | `0x4c2bec` | 0 | **DirectDraw driver GUID**, all 0 = primary (NULL) | `0x4027bd` |
| 0x2c..0x38 | 0x30 | `0x4c2bfc..c08` | 0x1ff, 1, 1, 3 | device caps flags and capability-derived choices no dialog shows (SETUP.md 1) | – |
| 0x3c | 0x40 | `0x4c2c0c` | 2 | Detect's "Effects quality" (outline, LIGHTING.md §5) | `0x42b380`, `0x43b43a` |
| 0x40, 0x44, 0x48 | 0x44 | `0x4c2c10/14/18` | 640, 480, 32 | display mode (DISPLAY.md §1) | `0x4027da`, `0x40614f` |
| 0x50 | 0x54 | `0x4c2c20` | 0 | Detect's "Activate VSync", inverted on NT (DISPLAY.md §2.2, SETUP.md 2) | `0x47ee16`, `0x47eea0` |
| 0x68..0x98 | 0x6c | `0x4c2c38..c68` | 0 (mkcfg bug, SETUP.md 4; Setup: 1, 1, 1, 0, …, 100, 30, 100, device) | sound page: switches Sound Fx / Music / Cinematic / Invert Left/Right, volumes (`0x4c2c50` sfx, `0x4c2c54` music, `+0x88` unused), output device, speakers (SETUP.md 1) | `0x4691e2`, `0x44fe2e` (bit 1 of `app+0x384`) |
| 0x9c | 0xa0 | `0x4c2c6c` | 0 | GUID of the joystick Setup found (`0x10001070`) | **not read**: the game enumerates itself |
| **0xac** | 0xb0 | `0x4c2c7c` | ↑ ↓ ← → Space LShift LCtrl LShift Enter Esc Num0 RCtrl | **keys config 1**, 12 × u32 | `0x44fc5a..` |
| **0xdc** | 0xe0 | `0x4c2cac` | R F D G, 0x200..0x207 | **keys config 2**, 12 × u32 | `0x44fd42..` |
| 0x10c | 0x110 | `0x4c2cdc` | 0 | Setup: a joystick was detected (`0x1000d6d8`) | – |
| 0x110 | 0x114 | `0x4c2ce0` | 0 | "Joystick Mode": radio "Digital" (0x45f) = 1, "Analogique" (0x460) = 0 (SETUP.md 1) | `0x44fc20` |
| 0x114 | 0x118 | `0x4c2ce4` | 1 | **keyboard only**: radio "Key" (0x46b) = 1, "Joy" (0x46a) = 0; forced 1 without a joystick | `0x44fc05` |
| 0x118 | 0x11c | `0x4c2ce8` | 0 | registry `HKCU\Software\Eko Software\The Gift` "Language" (Setup `0x100032a0`) | – |

The 12 key slots of each set, in cfg order, and the action they feed (`0x44fbd0`):

| slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| action | 2 ↑ | 3 ↓ | 0 ← | 1 → | 5 duck | 6 attack | 4 jump | 8 duck (riding) | 7 look | 9 pause | 10 camera | 11 special |
| config 1 (Setup `0x1000c030`) | DIK_UP | DIK_DOWN | DIK_LEFT | DIK_RIGHT | SPACE | LSHIFT | LCONTROL | LSHIFT | RETURN | ESCAPE | NUMPAD0 | RCONTROL |
| config 2 (Setup `0x1000c060`) | R | F | D | G | joy 0 | joy 1 | joy 2 | joy 3 | joy 4 | joy 5 | joy 6 | joy 7 |

`DefaultControlSettings` (`0x10002650`) copies these two tables; `0x100025d0` puts them into the struct before `SaveConfig`.
`GetStringFromCode` (`0x10001860`) also names codes "MOUSE BUTTON 1..4" and "JOY BUTTON 0..31"; the game has no mouse
bindings (a code in 0x100..0x1ff fails `0x467f70` and is never pressed).

## 3. Bindings and mode: `0x44fbd0` (app ctor, `[0x5e5814]`)

```c
app->mode /*+0x104*/ = cfg[0x114] == 1 ? 0 : cfg[0x110] == 0 ? 2 : 1;
memset(app->bind /*+0x108*/, 0, 0x1c * 4);                  /* 14 actions x 2 */
if (g_kbd) for (a, s) app->bind[a][s] = Code(cfg_key(a, s)); /* table in §2 */
Code(c) = c >= 0x200 ? c : (!g_kbd || g_kbd->vt2(c) == 0x90) ? 0x90 : g_kbd->vt2(c);   /* 0x44fe80: keys become internal codes */
KeyOf(a, s)   = app->bind[a][s];                             /* 0x44feb0 */
ButtonOf(a, s)= app->bind[a][s] >= 0x200 ? app->bind[a][s] - 0x200 : -1;   /* 0x44fed0 */
```
The only readers of `app+0x104` are `0x402d44` and `0x403018`, so modes 1 and 2 are the same in the game.

## 4. Per frame: `0x402940`

```c
kbd->vt3();                                               /* edges */
if (app+0x384 & 8) DebugKeys();                          /* dev mode: internal codes 0x41, 0x2b, 0x2c, 0x30, 0x12, … (camera, VM, cheats; 0x0f/0x10/0x11 = SavePos.bin, PERSO_DEATH.md §3.4); flag 8 is never set by the shipped exe */
if (rec->mode /*app+0x40 +4*/ == 1) { Replay(); return; } /* 0x402c8d: dev input replay instead of the devices (§4.2) */
Pad_Begin(dt);                                            /* 0x467340 */
if (state == 1 && !(app+0xf4 & 8) && perso && perso+0x5fc) Pad_Set(6, 1.0);   /* perso+0x5fc: an attack queued elsewhere (uncertain), cleared */
if (joy && (mode == 1 || mode == 2)) {                   /* 0x402d39 */
    joy->vt1();
    x = joy->vt3(); if (x > 0) Pad_Set(1, x); else if (x < 0) Pad_Set(0, x);
    y = joy->vt4(); if (y > 0) Pad_Set(3, y); else if (y < 0) Pad_Set(2, y);
    for a in {6, 11, 5, 4, 7, 10, 8, 9}:                  /* ButtonOf(a, 0) or ButtonOf(a, 1) */
        if (joy->vt5(ButtonOf(a,0)) || joy->vt5(ButtonOf(a,1))) { Pad_Set(a, 1.0); if (a == 4) Pad_Set(12, 1.0); }
}
if (kbd) {                                                /* 0x40300d; kbd->vt4 = held */
    if (mode == 0) { right: Pad_Set(1, +1); left: Pad_Set(0, -1); back: Pad_Set(3, +1); forward: Pad_Set(2, -1); }
    for a in {4, 5, 6, 11, 7, 8, 10, 9}: if (held(KeyOf(a,0)) || held(KeyOf(a,1))) { Pad_Set(a, 1.0); if (a == 4) Pad_Set(12, 1.0); }
}
if (Pad_Pressed(9) && (state == 1 ? Game_CanPause() /*0x445980*/ : state == 3)) Pause_Open();   /* 0x403331 → 0x404d80 */
/* 0x403360: the console "controller removed" check, dead on PC (§1.2) */
if (kbd && kbd->vt5(0x1b /*DIK_RETURN*/)) Pad_Set(0xc, 1.0);   /* 0x4033fb: Enter RELEASED = confirm */
Pad_End();                                                /* 0x467370 */
if (rec->mode == 2) Record(world->dt);                    /* 0x403424: dev input recording (§4.2) */
```
`Pad_Set(i, v)` = `0x4673b0`: held, value = v (the last call of a frame wins; in the original the stick and the direction keys
never both run, in the port's mode 3 a held key overrides the stick this way). `Game_CanPause` `0x445980`: Game state (`Game+0x14`) not 0/1/3/4, i.e. 2
(playing, iris open), and neither `0x44f2e0` (cinematic running) nor `0x44f2d0` on `Game+0x64`.

### 4.1 The stick in the Mover (`0x45a4b0`, PERSO_FRAME.md §2.3)
`x = Held(0) ? Value(0) : Value(1)`, `y = Held(2) ? Value(2) : Value(3)`; both 0 → no input. Otherwise the wanted direction is the
camera direction turned by `π − atan2(x, y)`, the target speed `(dot(new, old)+1)/2 · 600 · min(|(x,y)|, 1)` and the facing
turns by `slerp(old, new, |(x,y)| · 0.25)`. The keyboard always gives |x|, |y| = 1, so a key walks at full speed; the stick walks
slower with a small deflection. Race steering (`0x456210`), the side view (`0x45a7b0`) and the climbing state only look at
Held(0..3).

### 4.2 Input recording and replay (dev only): `0x406aa0`, object `app+0x40`
Round 34 (coverage triage). `app+0x40` is a 12-byte recorder `{u32 *frames, int mode, int index}`; the level load
`0x404234` picks the mode from the config word `cfg+0x378` (`[0x5e5814]`, set to **0** by the config ctor `0x44fb6c`, no
Woody.cfg field and no menu writes it, so the shipped game never records or replays):

| `cfg+0x378` | load | mode | what |
|---|---|---|---|
| 1 | `0x406930`: `frames = new(0xc8000)` | 2 = record | every frame after `Pad_End` `0x403434` → `Record(dt)` |
| 2 or 3 | `0x4069c0`: reads 5 + 4 + n·16 bytes from the file named at `cfg+0x278` (`0x43fd00`/`0x43fdb0`) | 1 = replay | `0x402c8d`: `Replay(0)` replaces all device input, `world->dt` = the recorded dt (`0x406a90`), the rest of the input step is skipped |

`Record` (mode 2, at most 0xc800 = 51200 frames): record `{u32 held, float dt, float y, float x}` with bit k = action k held
(`0x467400`), the analog values of actions 0/1 (x) and 2/3 (y) from `0x467460`. `Replay` (mode 1, no bound check):
`Pad_Begin(rec.dt)` `0x467340`, then `Pad_Set(k, value or 1.0)` `0x4673b0` for every set bit, `index++`. The level unload
`0x4049a0` writes the file in record mode (`0x406960`, `fopen(cfg+0x278, "wb")`: frame count, then the frames) or frees the buffer
in replay mode (`0x406a70`). Further hooks of the same dev feature: `0x401838` (dt = 1/`cfg+0x37c`, default 25, while
`cfg+0x378 == 2`), key code 0x3b in App::Frame `0x40167b` toggles `cfg+0x378` between 2 and 3, and with 3 during a replay
`0x4016c4` prints "Save bitmap %05d" (frame grabbing for videos). **Not needed by the port** (no player can reach it); the port's
own test input is `WOODY_KEYS` / `WOODY_PECKS` etc.

## 5. Menus and the pause menu

Menu input (MENU_NEWGAME.md §1.3): up/down/left/right = actions 2/3/1/0 just pressed (so the stick works in menus),
confirm = action 0xc just pressed (the jump key or its joystick button) or Enter released, back = action 5 just pressed
(duck key / button) or Esc released. The two fixed keys are DIK codes, not bindings.

### 5.1 `0x404d80` Pause_Open
```c
if (app+0x1c) { app->sound->vt[0x88]();  app->sound+0x10 = 0; }       /* suspend */
if (perso->state /*+0x21c*/ == 1 /*riding*/ && !app->blackbox /*+0xe4*/) {
    cam->0x41fa80(200.0f);                                   /* follow distance 200 (C+0x7e0/+0x7e4/+0x280) */
    Menu_Enter(0x19);
} else Menu_Enter(0x18);
App_SetState(0);                                             /* 0x401400: menu state, the world stands still (table 0x405af8) */
```
Also the way back from Options (`0x40575c`) and from "Are you sure?" No/back (`0x4057c7`).

### 5.2 Pages 0x18 / 0x19
Both are plain list pages (vtables `0x4aaa88` / `0x4aaa48`, created in `0x445e30` at `menu+0x60` / `+0x64`), y-fraction
0.05 (`0x4aab4c`), page-enter `0x45b390` (cursor on item 0, logo off). Items (`vt[2]`), 16 B each `{string | 0x20000, flags, result, 0}`:

| page | table | items (string id → result) |
|---|---|---|
| 0x18 | `0x4b5ce8`, 3 items (`0x45bd30`) | 4 Continue → 5, 36 Options → 6, 2 Quit → 7 |
| 0x19 | `0x4b5d18`, 4 items (`0x4600a0`) | 4 Continue → 5, **19 Start again → 18**, 36 Options → 6, 2 Quit → 7 |

Handler `0x4057f5` (jump table `0x405ce8` via byte table `0x405cfc[result − 5]`), shared by both pages; back does nothing:
```c
case 5:  if (Menu_Page() == 0x19) cam->0x41fa80(4.0f);             /* 0x40580c: the race follow distance again */
         App_SetState(app->blackbox ? 3 : 1);  break;
case 18: cam->0x41fa80(4.0f);                                      /* 0x40584d */
         Game_Respawn(game);                                        /* 0x445930 */
         Perso_RaceRestart(perso);                                  /* 0x4560f0 */
         App_SetState(1);  break;
case 6:  Menu_Enter(0x1b);  break;                                 /* 0x40587a */
case 7:  Menu_Enter(0x1c);  break;                                 /* 0x405888 */
```

### 5.3 "Start again": `0x445930` + `0x4560f0`
```c
void Game_Respawn(Game *g) {                         /* 0x445930 (also the respawn after a lost life, PERSO_DEATH.md §3.4) */
    Iris_Set(g+4, 0, 0, 0.1f);  g->state /*+0x14*/ = 0;  g->timer /*+0xc*/ = 0.1f;  [0x4b3354] = 0.2f;
    Perso_Respawn(g->perso, 0);                       /* 0x44a810: pos = +0x318, Reset (race: SurfEnter, state 1), ground snap */
    Actors_ResetAll();                                /* 0x40c040: a no-op, vtbl[28] = 0x445840 `ret` in every Npc class (PERSO_DEATH.md §3.4) */
    CamFollow_Reset(g+8);                             /* 0x458f90: hard cut to the follow camera */
}
void Perso_RaceRestart(Perso *p) {                   /* 0x4560f0 */
    p->hasCheckpoint /*+0x330*/ = 0;  p->bonusAtCheckpoint /*+0x4e0*/ = 0;
    p->respawnPos /*+0x318*/ = p->startPos /*+0x30c, set at Perso init 0x44a44a and level restart 0x44a6e6*/;
    Perso_Respawn(p, 0);
    if (p->board /*+0x498*/) { p->board->Reset(); p->board->Request(0); }
}
```
The Game sequence (PERSO_FRAME.md §4.1) is then in state 0 with 0.1 s to go and the iris shut: after 0.1 s `0x445930` runs
once more (at the start position now), the iris opens 0 → 1 in 1 s (state 1), then state 2. **No life is lost** (only
state 4 does that). The facing `+0x324` is not reset, but race sub-state 0 takes the ride direction from the track when
there is no checkpoint (RACE.md §4.2).
So the restart puts back only the rider: enemies, projectiles and ordinary bonuses stay as they are (`0x40c040` does nothing);
the race bonuses come back and the count restarts from 0 through Reset → SurfEnter (`0x44f8a0`, `+0x264 = +0x4e0 = 0`), and
`[0x4b3354]` is never read.

## 6. The port

`src/main_engine.c` (block "input", after the options), `src/render_gl.c` (window keys), `src/player.c` (`player_restart`, the stick in
the Mover).

- **Woody.cfg**: `in_read_cfg` looks at `WOODY_CFG`, then `Woody.cfg` in the working directory (the original's place), then
  `<Data>/../Woody.cfg` (the install layout). Magic checked, both key sets and the mode read; DIK codes become VK codes
  (`in_dik_vk`: a table for the extended keys and the numpad, `MapVirtualKey` for the rest, so the keyboard layout counts
  like DirectInput's physical keys). The volumes stay in `woodyre.cfg` (MENU_OPTIONS.md); nothing is written to Woody.cfg.
  The same file lookup (`wcfg_read`) also feeds `setup_import`: vsync, reverse stereo and film sound for `woodyre.cfg`
  keys that are still missing (SETUP.md 2.4, 3).
- **Without Woody.cfg** the port keeps its own keys: arrows/WASD, Space jump, X duck (and duck while riding), LCtrl/Shift
  attack, Enter look, Esc pause, C/Num0 camera, RCtrl/E special; joystick buttons as Detect's config 2 (§2: button 0 duck,
  1 attack, 2 jump, 3 duck riding, 4 look, 5 pause, 6 camera, 7 special), and **mode 3** = keyboard and joystick both
  give directions (port choice). With a cfg in a joystick mode but no joystick present the keys give directions too (the
  original would leave you without directions).
- **Joystick**: WinMM `joyGetPosEx` on the first device that answers (ids 0..15, looked for again every 3 s while none,
  so a pad can be plugged in later); X/Y are mapped to ±4096 and put through the dead zone of `0x467a80`; buttons = the
  low 32 bits of `dwButtons`. Mode 0 never polls it (as the original).
- **Pads (PORT EXTRA, `src/pad.c`)**: the controllers of today, read as raw HID with `hid.dll` / `setupapi` (no driver, no
  Steam): DualSense / DualSense Edge and DualShock 4 over USB and Bluetooth; and XInput (`xinput1_4.dll`, else 1_3 / 9_1_0,
  loaded at run time; ordinal 100 = XInputGetStateEx for the Guide button) for Xbox pads and everything that emulates one,
  empty slots asked every 2 s and on a device change because that call is slow. Report layouts as in SDL's hidapi drivers: DualSense
  input 0x01 (USB) / 0x31 (Bluetooth, CRC-32), which Bluetooth only sends after a feature report is read (0x09 serial, 0x20
  firmware); DualShock 4 0x01 / 0x11..0x19 (feature 0x05). They are looked for at start and on every `WM_DEVICECHANGE`
  (`Window.dev_changes`, 0.5 s and 2 s later), all of them act as one pad, and nothing is read while the window is not in
  front. Rumble on XInput: both motors get the same value (`XInputSetState`). Mapping (`g_in.pbind`, separate from the Woody.cfg keys): left stick = the joystick axes through the dead zone of
  `0x467a80` (`pad_deadzone=` in woodyre.cfg, default the original's 30 %), D-pad = directions, A/Cross 4 (and confirm),
  B/Circle 5 and 8 (and back), X/Square and RT 6, RB 7, Start/Options 9, LB and R3 10, Y/Triangle and LT 11. The right stick
  adds look-around counts (`ftol(value · 5)`, the same weight as the left stick, §6 mouse); outside the look-around it turns
  the follow camera round Woody (x, up to 2.6 rad/s, stick right = the view turns right) and lowers / raises it (y, 400 units/s,
  -120..+350 on top of the height of message 670, kept until "camera behind"), both scaled by the Controls page's "Camera
  speed" (25/50/75/100/150/200 %, `camera_speed=` in woodyre.cfg, default 100) and both as part of the camera's own move in
  `camera_step`, so the sweep and the line-of-sight veto of CAMERA.md still stop it at walls; not in behind mode (climbing,
  rides, races, the behind key). Test hook `WOODY_RSTICK="T:RX:RY[:D] ..."`. While a pad is connected the
  WinMM joystick is not used (it would be the same pad again; Sony devices, `wMid` 0x054c, are skipped anyway).
- **Rumble (PORT EXTRA)**: the original's calls of `0x44d1b0(a, b)` (a = strength, b = seconds; the empty `0x467b20` never
  used them) now reach the pads, scaled by the Vibration option (MENU_OPTIONS.md 5.2): hit (0.5, 0.5), lightning / laser
  (0.5, 1.5), bomb or rocket blast (1.0, 0.5), fall damage (0.8, 0.5), attack hit (0.5, 0.3), wall / ground rebound
  (0.5, 0.5), charge run every 0.2 s (0.5, 0.15, clock `+0x600`), special (1.0, 1.0), race crash (0.5, 0.5, at the crash and
  on landing). Not while dead (`+0x21c == 2`). Overlapping calls: the strongest running one wins. The DualSense light turns
  Woody red while the game runs (`WOODY_PADNOLIGHT=1` leaves it alone) and goes back to the system's at exit.
  Testing: `WOODY_PAD="T:LX:LY:BUTTONS[:D] ..."` (as `WOODY_JOY`, buttons `1 << PAD_*` of `src/pad.h`), `WOODY_PADLOG=1`
  logs every rumble call.
- **Linux / SDL2 build (`src/pad_sdl.c`, `src/plat_sdl.c`, docs/PLATFORM.md)**: the same `pad.h` on SDL's game controller
  API (DualSense / DualShock 4 with rumble, light bar and player light, Xbox pads, the Steam Deck through Steam Input), the
  same merge, focus pause and rumble mixing; no WinMM joystick. Keys arrive as SDL scancodes and are stored as the VK codes
  above (letters by the layout), so bindings, woodyre.cfg and the Controls page are the same on both. Testing:
  `WOODY_VPAD="T:LX:LY:BUTTONS[:D] ..."` attaches an SDL virtual controller (buttons `1 << SDL_CONTROLLER_BUTTON_*`), the
  whole SDL path; `WOODY_KEYLOG=1` prints every key event.
- **Controls page and woodyre.cfg bindings (PORT EXTRA)**: Options → Controls (menu page 0x41, the list class of the Display
  page 0x40) shows a device choice (Keyboard / Controller) and a row per action: walk forward / back / left / right, jump,
  attack, special, duck (actions 5 and 8 together), look around, camera behind, pause. Confirm on a row waits for a key or
  button (first until everything is let go): one already in the row is taken out, any other is added and taken out of the
  other rows (Shift also matches Left / Right Shift); Esc or 6 s without input leaves the row. Defaults puts back the port's
  keys or buttons of that device, Continue saves, back undoes the visit. The edits are written to woodyre.cfg as
  `key_<row>=Up arrow,W` and `pad_<row>=X,RT` (rows `forward back left right jump attack special duck look camera pause`,
  key names as on the page, WinMM buttons `Joy1..Joy32`, pad buttons by their Xbox names `A B X Y LB RB LT RT Back Start LS
  RS Up Down Left Right Guide Touchpad`); they win over Woody.cfg (`ctl_apply_cfg` after `in_read_wcfg`), which is only read
  while woodyre.cfg has none. The menus always take the arrow keys too, whatever the bindings. The page has 15 rows: the
  menu renderer shrinks a page's font until its last row fits (no page of the original needs that).
- **Actions**: `in_frame` builds the 14 actions per frame in the order of `0x402940`; `PlayerInput` gets held states and
  the stick (`ax`, `az`), which scales speed and turn rate in `player_update` exactly like `0x45a4b0`. Menu keys use the
  actions (confirm = action 12 pressed or Enter released, back = action 5 pressed or Esc released, pause = action 9).
- **Pause**: `pause_open` = `0x404d80` (page 0x19 while riding, camera distance 200; the pause is only possible in Game
  state 2 like `0x445980`); the handler does Continue / Start again / Options / Quit; `player_restart` = `0x445930` +
  `0x4560f0` (Player `start_pos` = `+0x30c`).
- Tests: `WOODY_KEYS` as before (new names RCTRL, SHIFT, NUM0 and single letters; `CTRL` counts as the left Ctrl);
  `WOODY_JOY="T:X:Y:BUTTONS[:D] ..."` pushes the stick to X,Y (−1..1, before the dead zone) with a button mask for D s
  (default 0.08) from T s on the level clock, in place of the device.

The bonus-at-checkpoint `+0x4e0` is ported (`race_bonus_ckpt`: set by 1030, zeroed by `player_restart`, restored by
`race_enter`); `Actors_ResetAll` `0x40c040` and `[0x4b3354]` need nothing (a no-op and a write-only global, §5.3).
- **Mouse**: the original's mouse never reaches the game (§1.3), so by default the port's look-around gets 0 counts too.
  `src/render_gl.c` registers the mouse as raw input (`RegisterRawInputDevices`, usage page 1 / usage 2, no flags = only while
  the window is in the foreground, cursor untouched, like coop level 6) and sums `WM_INPUT` relative motion into
  `Window.raw_dx/raw_dy`, reset by every `win_poll` (the counts since the last poll, as `GetDeviceState` would give).
  `WOODY_LOOKMOUSE=1` (port extra) feeds them into the look-around's `L+0x28/+0x2c` in place of the 0, where the original's own
  formula applies (clamp ±64, `counts·dt·π/16` per frame, at most π/10; no button needed, the keys override it per axis);
  `WOODY_MOUSE="T:DX:DY[:D] ..."` adds DX, DY counts per frame for D s (default 0.5) from T s on (testing). The window-pixel
  deltas with the right button (`Window.mouse_dx/dy`) now only drive the F5 free camera.

Not ported: the debug keys (dev flag 8, never set by the shipped exe; they include `SavePos.bin`, PERSO_DEATH.md §3.4),
DirectInput's exclusive mode, and the console "controller removed" page 0x1a.
