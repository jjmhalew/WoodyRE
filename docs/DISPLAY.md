# DISPLAY.md — display mode, frame pacing, and the port's display options (Woody.exe, build 17-10-2001)

§1–§2 are static analysis of `out/disasm_full.txt` (imagebase 0x400000) and of `game/Setup.dll` (capstone, imagebase
0x10000000); nothing was traced. §3–§6 describe the port: **everything in §3–§6 is a port extra** (issue #12); the original
has no display options in the game itself. Related: MENU_OPTIONS.md (page 0x1b), INPUT.md §2 (Woody.cfg layout),
CAMERA.md §5 (projection), HNM.md (films).

## 0. Summary

- **Display mode**: always **exclusive fullscreen** at the Woody.cfg mode (`[0x4c2c10]` w, `[0x4c2c14]` h, `[0x4c2c18]` bpp;
  Setup.dll default **640x480x32**, other modes from Detect.exe's "Display Mode" list). The windowed code path exists
  but its switch (`app+0x384` bit 0) is never set in the shipped build.
- **Pacing**: the main loop has **no Sleep, no timer wait and no frame cap**. The only brake is the present:
  `Flip(DDFLIP_WAIT)` (= wait for vertical blank) or, with the other value of the VSync flag, a `Blt` with
  `DDBLTFX_NOTEARING`. The flag is Detect's **"Activate VSync"** box (SETUP.md 2); with the cfg every installation on the
  primary display driver gets (flag 0), Windows NT/2000/XP and later invert it to 1 at device creation, so the game
  **flips on vsync** by default there.
- **dt**: raw `QueryPerformanceCounter` delta per frame, **clamped to 0.1 s** (and a dt ≤ 0 also becomes 0.1). A developer
  "Constant Frame rate" switch replaces it with `1/200` s.
- **Aspect**: the layout (HUD, menus: 640x480 virtual) and the 3D projection are fixed 4:3; a non-4:3 mode is stretched.
- **Port**: window of any size or borderless fullscreen, 4:3 (pillarboxed) or wide (Hor+, menus kept 4:3 and centred, HUD at the edges),
  vsync (default on, as the original), optional fps cap, all saved in `woodyre.cfg`; a "Display" page under Options.
  A "Graphics" page adds ambient occlusion, texture sharpness (anisotropic filtering) and edge smoothing (SMAA, MSAA),
  all off by default, which is the original's look (§5).

## 1. The original: display mode

### 1.1 Window and device
- Window (`0x4060c0`): `CreateWindowExA(0, "Trip to world", "Trip to world", WS_POPUP, …)` of `(w + 4) x (h + 0x18)`, centred
  on the desktop rectangle (`0x40612d..0x4061a8`, `[0x4c2c10]`/`[0x4c2c14]` at `0x40614f`/`0x406159`).
- Renderer object `new(0x48)` → ctor `0x47e6f0` (`[0x5e8650] = this`), created from `0x402760`:
  `byte [renderer+0] = app+0x384 & 1` (**windowed**, `0x4027b2..0x4027bb`). The app ctor `0x44fa40` clears bits 0..4
  of `app+0x384` (`0x44fa54`) and the only other writer (`0x44fe5f`) sets bit 1 (sound); **bit 0 is never set**, so the
  shipped game always takes the fullscreen path. (The resource dialog with "In window", "Constant Frame rate", "Frame
  rate", "Record game" is a developer leftover, see §2.3.)
- Device init `0x47e8c0(hwnd [0x4c3a94], ddraw GUID or NULL, &d3d GUID 0x4c2bdc, w, h, bpp, 16)` (`0x4027da..0x402829`):
  `DirectDrawCreateEx` (`0x4913e6`), then
  - fullscreen (`byte [ebp] == 0`, `0x47e961`): `SetCooperativeLevel(hwnd, 0x811` = FULLSCREEN | EXCLUSIVE | FPUSETUP`)`,
    **`SetDisplayMode(w, h, bpp, 0, 0)`** (vt 0x54, `0x47e980`; refresh rate 0 = the driver default); primary surface with
    **one back buffer** (flags 0x21, caps 0x2218 = PRIMARYSURFACE | FLIP | COMPLEX | 3DDEVICE, count 1, `0x47ea8f..0x47eab7`),
    back buffer via `GetAttachedSurface` (`0x47eae2`);
  - windowed (never used): `DDSCL_NORMAL | FPUSETUP` (0x808), a primary plus an off-screen back buffer and a clipper.
  - The 7th argument **16** is the **z-buffer depth** (`[ebp+0x28]` → `DDPIXELFORMAT.dwZBufferBitDepth`, flags
    `DDPF_ZBUFFER` 0x400, caps 0x24000 = ZBUFFER | VIDEOMEMORY, `0x47eb10..0x47eb4b`), **not** the display bpp: the display
    mode uses the cfg bpp (32 from Setup). (TRACING.md §2 used to call it "forces 16 bpp".)
- The cfg mode comes from Setup.dll/Detect.exe: Setup's defaults at `0x100032dc..0x100032f0` are 640, 480, 32 and
  VSync flag 0 (`0x100032fa`); its per-device mode table (`0x100247ac`, 12 B per entry: w, h, bpp) fills the
  "Display Mode" combo of Detect.exe; the chosen entry is copied into the cfg at `0x10003a9d..0x10003abb`.

### 1.2 Aspect ratio
The HUD/menus draw in a fixed 640x480 virtual space (`RectVirtual`, HUD_TEXT.md §5) and the projection has fixed
`sx = 1.0`, `sy = 0.75` (CAMERA.md §5.1: hfov 100.4°, vfov 84.0°); `0x41f690` takes the full viewport whenever
`W/H > aspect`. So a non-4:3 mode picked in Detect (e.g. 1280x1024, or a widescreen mode on a driver that lists one) is
**stretched**, both the 3D view and the 2D layer. No Hor+ or pillarbox code exists.

## 2. The original: frame pacing

### 2.1 Main loop (`0x405ea1`, WinMain)
```c
for (;;) {
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto quit; TranslateMessage; DispatchMessageA; }
    if (!app_active /*[0x4c3a9c]*/) { WaitMessage(); continue; }            /* 0x405f41: only an inactive app blocks */
    if (App::Frame(app) /*0x401590*/ == 1) { WriteCfg("Woody.cfg") /*0x401130*/; PostMessageA(hwnd, WM_CLOSE, 0, 0); }
    ...                                                                      /* 0x405f09: the developer dialog, if requested */
}
```
No `Sleep`, no `timeGetTime` wait: the six `Sleep` and seven `timeGetTime` calls of the exe are all in the sound/stream
library (`0x48ccfd..0x4974d3`), not in the frame path.

### 2.2 Present `0x47ee90` (called by App::Frame at `0x401746`, and `0x401609` on the frame-skip path; `0x47ef80` = clear + present)
```c
void Present(Renderer *r) {                       /* 0x47ee90 */
    if (!r->windowed) {
        if (g_vsync /*[0x4c2c20]*/) primary->Flip(back, DDFLIP_WAIT /*1*/);          /* 0x47eeb4: waits for the vertical blank */
        else { DDBLTFX fx = { sizeof fx /*0x64*/, DDBLTFX_NOTEARING /*8*/ };
               primary->Blt(NULL, back, NULL, DDBLT_DDFX /*0x800*/, &fx); }           /* 0x47eef4: no DDBLT_WAIT */
    } else { ClientToScreen; OffsetRect; primary->Blt(&dst, back, &src, 0, NULL); }  /* 0x47ef01: windowed, never used */
}
```
`[0x4c2c20]` = Woody.cfg `+0x50` (file offset 0x54), Detect.exe's checkbox 0x3f2. Its dialog template says "Disable VSYNC"
(file offset 0x354ee of Detect.exe), but `OnInitDialog` replaces that with string 0x6f **'Activate " VSync"'**
(`0x402c68..0x402c84`), so 1 = "vsync wanted" (SETUP.md 2.1). The Setup.dll side keeps it per device in
`[0x10033dec + 4·dev]`: 1 for a device on a secondary DirectDraw driver, 0 on the primary one (`0x1000363f..0x10003651`,
SETUP.md 2.3). At the end of device init (`0x47ee0e..0x47ee22`): **if `[0x4c3aa0]` (1 on a Windows NT platform, set by
WinMain at `0x405ddf` from `GetVersionExA`; 0 on Win9x at `0x405dbe`) then `[0x4c2c20] = 1 − [0x4c2c20]`**. Consequences:

| platform | cfg "Activate VSync" | `[0x4c2c20]` | present |
|---|---|---|---|
| NT / 2000 / XP and later | 0 (default on the primary driver, mkcfg) | 1 | Flip, **vsync** |
| NT / 2000 / XP and later | 1 | 0 | Blt + NOTEARING (driver dependent, in practice no wait) |
| Win9x | 0 | 0 | Blt + NOTEARING |
| Win9x | 1 | 1 | Flip, vsync |

**Settled**: the box works as labelled on Win9x and inverted on NT. The inversion is applied in place to the global the
game writes back to Woody.cfg on a normal quit (`0x401130` writes the whole block `0x4c2bd0`), so on NT every session that
quits that way stores the opposite flag and the next one uses the other present mode — not a consistent design, whatever
the intent was (SETUP.md 2.2). Only the NT rows matter for a modern system. With exclusive fullscreen,
a flip chain of one back buffer and DDFLIP_WAIT, the frame rate is the refresh rate of the display mode (or an
integer fraction of it when a frame takes longer). The known "no VSync" complaint (ANALYSE.md) concerns wrappers
and the Blt path, not the default.

### 2.3 dt `0x401810` (App::Frame, before or after the input step `0x402940` depending on the state)
```c
float dt = qpc_delta();                          /* 0x42a3f0: (now - prev) / QueryPerformanceFrequency (init 0x42a3c0) */
if (g_const_rate /*[0x5d7b89]*/) dt = 1.0f / g_rate /*[0x4b3a8c] = 200*/;   /* dev: "Constant Frame rate" (dialog 0x4488aa, debug key 0x13 at 0x402b7f) */
if (rec->mode /*app+0x44 +0x378*/ == 2 && rec->rate /*+0x37c*/) dt = 1.0f / rec->rate;              /* replaying a recorded game */
if (!(dt <= 0.1f) || !(dt > 0.0f)) dt = 0.1f;   /* 0x40185b: clamp to 0.1 s (0x4a9008); dt <= 0 also becomes 0.1 */
world->dt /*+0x38*/ = dt;                        /* initialised to 1/30 at 0x42a474 */
```
So the game speed is real time down to 10 fps, slow motion below. The port does the same clamp (`main_engine.c`, the
`dt > 0.1f` line); the dt ≤ 0 case cannot occur with QPC.

## 3. Port: display (PORT EXTRA)

- **Window / fullscreen** (`render_gl.c` `win_mode`): a normal window whose client is W x H, centred on the work area and
  shrunk (aspect kept) when it does not fit; or **borderless fullscreen** (`WS_POPUP` over the monitor, desktop
  resolution — "native", e.g. 3840x2160 on a 4K screen). The process is DPI aware (`SetProcessDPIAware`), so a scaled
  desktop gives real pixels. No display-mode switch (the original's exclusive mode) is done. F11 toggles fullscreen for
  the running session.
- **View box** (`main_engine.c` `disp_view`): the 3D view is drawn into a box of the window (`Window.vx/vy` +
  `width/height` handed to `rnd_frame`): **4:3** = the largest centred 4:3 box, black bars left/right (like the HNM films,
  `rnd_film_frame`); **wide** = the whole window. A window narrower than 4:3 is letterboxed in both modes.
- **Hor+**: the projection keeps the original's vertical fov (84.0°, 68.0° in the 16:9 letterbox strip) and takes the
  aspect of the view box, so a wide view sees more to the sides (16:9: hfov 116°, 4:3: 100.4° as the original).
- **2D layer** (`hud.c` `hud_begin_view`): the 480 virtual lines fill the view height; on a view wider than 4:3 the
  virtual x range grows symmetrically around 0..640 (16:9: −107..747), so every HUD element and menu page keeps its
  4:3 shape; menu pages sit centred, while the in-game HUD hugs the view's edges (`hud_edges`): the blue bar, the
  bonus / $ / charge column and the power gauge move by the left edge `vx0`, the red bar, hearts, portrait, lives and
  the boss row by the right edge `vx1 − 640` (slide-outs and the W swarm follow). `hud_rect` (menu backdrop, storm flash, black of page 4) covers the full range; the iris
  ring reaches the corners of the wider view and is fully open at v = 1. Pickups projected to the screen
  (`rnd_project`, the HUD fly-in) are remapped into that range. `hud_bars` paints the pillar/letter bars black at the
  end of the frame (after the fade).
- **Vsync**: `wglSwapIntervalEXT(1)` by default (the original's NT default, §2.2), 0 = off. Until `woodyre.cfg` has a
  `vsync=` key, the port takes it from Woody.cfg `+0x50` with the NT rule, vsync = (flag ≠ 1) (`setup_import`, SETUP.md 2.4).
- **Fps cap**: optional (default off, as the original); Sleep(1) with `timeBeginPeriod(1)` until 2 ms before the due
  time, then a spin. The dt clamp stays.

## 4. Port: the Display page (PORT EXTRA)

Options page 0x1b gets a sixth item **"Display"** after "Continue" (y 424.5, the proposal of MENU_OPTIONS.md §9.4; the
original five stay where they were). "Graphics" (§5) and "Controls" (INPUT.md) follow it; with eight items the page starts
at y-fraction 0.17 instead of 0.4. "Display" opens port page **0x40**, the same list class (size 30, centred, white, 2 Hz
blink, no cursor), y-fraction 0.25:

| i | text | flags | left/right |
|---|---|---|---|
| 0 | Display | 2 header | |
| 1 | Aspect ratio 4:3 / Wide | 0x100 choice | toggles |
| 2 | Window size WxH | 0x100 | 640x480, 800x600, 1024x768, 1280x960, 1280x720, 1280x800, 1600x900, 1920x1080, 2560x1440, 3840x2160 (round) |
| 3 | Fullscreen On / Off | 0x100 | toggles (Common 133 "On" / 134 "Off") |
| 4 | VSync On / Off | 0x100 | toggles |
| 5 | Frame rate limit Off / N | 0x100 | Off, 30, 60, 120, 144, 240 (round) |
| 6 | Continue | 1 | confirm: apply + save, back to 0x1b on "Display" |

Flag **0x100** (port) draws `name + space + string(value)` where `value` is a string ref. Port-only words are ASCII
turned into the font's codes (`hud_port_str`, refs `0x7f000000 | n`, the glyph order of HUD_TEXT.md §1.4; all needed
characters exist). Left/right blink like a slider step (phase 0.25). **Back** drops the edit; nothing is applied before
Continue. In a level the page has the half-black backdrop and pauses the world like 0x1b.

## 5. Port: the Graphics page (PORT EXTRA)

Options → **Graphics** opens port page **0x42**, the class of the Display page (y-fraction 0.25, left / right change a
choice, Continue applies at once without touching the window and saves, back drops the edit):

| i | text | woodyre.cfg | choices |
|---|---|---|---|
| 0 | Graphics | | header |
| 1 | Ambient occlusion | `ao=` | Off, On |
| 2 | Texture sharpness | `aniso=` | Original (1), 2x, 4x, 8x, 16x |
| 3 | Edge smoothing | `smaa=` | Off (0), Low, Medium, High, Ultra (1..4) |
| 4 | Multisampling | `msaa=` | Off (0), 2x, 4x, 8x |
| 5 | Continue | | |

A choice the GL cannot do shows **"Not supported"** (and the lists stop at what the driver offers). The Android and
Switch builds draw through the GLES 2 shim of `src/gles`: there only texture sharpness can work (when the driver has
`GL_EXT_texture_filter_anisotropic`). Everything after the 3D picture - HUD, menus, text, fades, films - is drawn into
the window directly and is never smoothed or darkened.

- **Ambient occlusion** (`gtao.c`; the original has nothing like it, its world light is the baked `.lit` polygons,
  LIGHTING.md): GTAO (Jimenez et al. 2016, in the form of Intel's XeGTAO) as a screen-space pass over the opaque image.
  `rnd_frame` calls `gtao_frame` at the end of pass 0, after the opaque world and models and before the additive world
  faces, the water, the fade list and the sprites, so glow, water and HUD are never darkened. The depth of the 3D
  viewport is copied into a depth texture (with MSAA: resolved by a depth blit into a depth / stencil texture); a
  GLSL 1.20 pass into an RGBA8 framebuffer object takes the view-space position and normal from the depth and integrates
  the visible arc of 3 slices x 6 steps per side within 120 world units (Woody is 193 tall; the screen radius is capped
  at a quarter of the view height), slice rotation and step offset from a 4x4 Bayer tile; a 4x4 depth-aware box blur
  (one noise tile) multiplies `visibility^1.5` into the frame (blend ZERO / SRC_COLOR).
- **Texture sharpness** (`render_gl.c`, `rnd_set_aniso`): `GL_TEXTURE_MAX_ANISOTROPY` on every level texture (world,
  models, sky, texture-pack replacements), on top of the original's own filtering (its 4-level mip chain with
  `GL_LINEAR_MIPMAP_NEAREST`, MODEL_RENDER.md): floors and walls seen at a grazing angle stay sharp further away. A change
  is applied to the loaded textures at the next frame.
- **Edge smoothing / multisampling** (`postfx.c`): with either on, `postfx_begin` (main loop, before `rnd_frame`)
  binds an own target of the window's size (colour RGBA8, depth 24 + stencil 8 for the cast shadows) and the 3D picture
  is drawn into it as into the window; `postfx_end` (after `rnd_sorted`, before the 2D layer) puts it into the window.
  **MSAA**: the target is multisampled and resolved with `glBlitFramebuffer` - polygon edges only, the colour-key
  cut-outs (foliage, fences) stay hard. **SMAA** (Jimenez et al. 2012, the 1x mode of `src/smaa/smaa.h`, which
  `tools/smaa_embed.py` makes from iryoku/smaa, MIT): colour edge detection, blending weights from the edge shapes with
  the AreaTex / SearchTex lookups (embedded as PNG, decoded with stb_image), neighbourhood blending into the window; the
  presets are SMAA's own (Low / Medium / High / Ultra). It also smooths the cut-outs, and runs after the MSAA resolve when
  both are on. SMAA needs GLSL 1.30, MSAA framebuffer objects with multisample renderbuffers (GL 3.0).
- **Cost** (RX 6800, 1920x1080, vsync off; the engine is CPU bound, so most of it hides behind the CPU): W1A 1.54 ms per
  frame with everything off, 1.61 with ambient occlusion, 1.57 with SMAA High or MSAA 4x, 1.90 with everything at
  maximum; WWS 0.81 / 1.27 / 0.81 / 1.62 ms.

## 6. Port: storage and overrides (PORT EXTRA)

`woodyre.cfg` (key=value, read at boot before the window opens, written on Continue and at exit):
```
aspect=wide        # or 4:3
window=1280x800
fullscreen=0
vsync=1
fpscap=0           # 0 = off
ao=0               # the Graphics page (§5): ambient occlusion 0/1
aniso=1            # texture sharpness 1 (the original), 2, 4, 8, 16
smaa=0             # edge smoothing 0 = off, 1..4 = low, medium, high, ultra
msaa=0             # multisampling 0 = off, 2, 4, 8
```
Defaults = the port's window before these options (1280x800, wide, windowed, vsync on, no cap; vsync from Woody.cfg while
the key is missing, §3) and the Graphics page all off. The same file also keeps `reverse_stereo=` and `film_sound=`
(SETUP.md 3). Overrides: `--res WxH`, `--windowed`, `--fullscreen`, `--aspect 4:3|wide`, `WOODY_VSYNC=0/1`,
`WOODY_FPSCAP=N`, `WOODY_AO=0/1`, `WOODY_ANISO=N`, `WOODY_SMAA=0..4`, `WOODY_MSAA=0/2/4/8`. A screenshot run
(`--shot`, `WOODY_SHOTSEQ`, `WOODY_LOGOSHOT`) ignores the cfg's display keys and starts from the defaults, so test images
stay 1280x800 wide unless the command line says otherwise. `WOODY_FPS=N` (testing: frame-rate dependent code) still
takes precedence over the cap. `WOODY_FPSLOG=1` prints the frame rate every 2 s. `WOODY_FIXDT=N` (testing) advances the
game clock by exactly 1/N s per frame whatever the wall clock says, and the hooks timed from the level start (`--shot`,
`WOODY_SHOTSEQ`, `WOODY_KEYS`) follow that clock, so two builds produce the same frames: diff their screenshots pixel for
pixel (renderer changes that must not change the picture).

## 7. Uncertain

1. ~~Whether the Win9x/NT inversion of the VSync flag is deliberate; the Setup.dll per-device default~~: settled in §2.2
   and SETUP.md 2 (label "Activate VSync"; right on Win9x, inverted on NT, re-inverted into the cfg at every quit; default
   1 only for devices on a secondary DirectDraw driver).
2. What `DDBLTFX_NOTEARING` did on the drivers of the time (documented as "schedule the blit to avoid tearing"; many
   drivers ignored it).
3. The frame-skip path of App::Frame (`0x4015d6..0x401609`: states with `app+0xf4` bit 1 set and bit 2 clear present
   without drawing) was not followed further.
