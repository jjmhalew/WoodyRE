# WoodyRE

A reimplementation of the engine of *Woody Woodpecker: Escape from Buzz Buzzard Park* (PC, Eko Software / Cryo, 2001),
rebuilt by reverse engineering the original. It runs the original game data from **your own CD**: no game files are
included in this repository or its releases.

<p align="center">
  <img width="100%" alt="Woody's special attack flattens three pirate chickens (W2A)" src="https://github.com/user-attachments/assets/caf51280-5d79-4810-a16f-c9ee0cfb5059" />
</p>
<p align="center">
  <img width="49%" alt="Woody rides a rocket (W1A)" src="https://github.com/user-attachments/assets/7e15997d-394b-4527-b234-ff6f9551859f" />
  <img width="49%" alt="The new-game intro: Buzz and his sidekick climb in through the window" src="https://github.com/user-attachments/assets/6f72f708-64b2-42aa-92bf-c0c5511599aa" />
</p>

The new-game intro with sound:

https://github.com/user-attachments/assets/a6b3fd6f-4642-4c40-9a26-c97c50b65f9a

> Unofficial fan project for preservation. Not affiliated with or endorsed by Eko Software, Cryo Interactive, Microïds,
> Universal Studios or Walter Lantz Productions. Woody Woodpecker and related names are trademarks of their owners.
> You need your own copy of the original game to play.

## Playing
1. Get `WoodyRE.exe`: download it from the [Releases](../../releases) page, or build it yourself (below).
2. Put it in a folder of its own and start it. (The exe is not signed, so Windows SmartScreen may say "Windows protected
   your PC" at the first start: *More info*, then *Run anyway*.)
3. At the first start it looks for the game CD in every drive (a mounted ISO image works too), asks once, copies the game
   files (about 640 MB) into `data\` next to the exe and checks each one against the English 1.00 CD. After that the CD
   is no longer needed. No CD drive? Choose a folder with a copy of the CD's files instead, or copy `Data`, `Common`,
   `Logo`, `Game` and `Music.bf` from the CD into `data\` yourself.

**Linux and Steam Deck:** download the `linux-x86_64.tar.gz` from the Releases page (or run `./build.sh`), unpack it and
start `woodyre`. It needs SDL2 (`libsdl2-2.0-0`; SteamOS and most desktops have it). At the first start it looks for the
CD under `/media`, `/run/media` and `/mnt` and copies the game files into `~/.local/share/WoodyRE/data` (the window opens
when the copy is done); or copy `Data`, `Common`, `Logo`, `Game` and `Music.bf` there yourself. `woodyre.cfg`, `woodyre.sav`,
`woodyre.log` and `mods/` then live in `~/.local/share/WoodyRE` (with the game files in a `data/` folder next to `woodyre`
instead, they live next to `woodyre`, or in `~/.local/share/WoodyRE` when that folder is read-only). On the Steam Deck add `woodyre` to Steam as a non-Steam game; the
Windows `WoodyRE.exe` under Proton should work as well.

**Android** (7.0 or later, 64-bit phones and tablets, OpenGL ES 2.0): install `WoodyRE-<version>.apk` from the Releases
page (allow installing from your browser or file manager when Android asks), or build it (below). At the first start
choose an ISO image of the CD, or a folder with a copy of its files, in Android's file picker; the game files are copied
into the app's own folder `Android/data/io.github.jjmhalew.woodyre/files/data` and checked as on Windows. You can also
copy `Data`, `Common`, `Logo`, `Game` and `Music.bf` there with a USB cable. `woodyre.cfg`, `woodyre.sav`, `woodyre.log`
and `mods/` live in `Android/data/io.github.jjmhalew.woodyre/files`. A Bluetooth or USB pad works like on Linux and hides
the touch controls; touching the screen brings them back.

**Nintendo Switch** (experimental; needs a Switch that runs homebrew, i.e. custom firmware such as Atmosphère): put
`woodyre.nro` from the Releases page (or `./build_switch.sh`) into `/switch/woodyre/` on the SD card. Put an ISO image of
the CD in the same folder: the first start unpacks it into `/switch/woodyre/data` (a few minutes, once), then asks you to
press + and start WoodyRE again; the ISO can be deleted then. Or copy `Data`, `Common`, `Logo`, `Game` and `Music.bf` from the CD into `/switch/woodyre/data` yourself.
Start it from the Homebrew Menu, preferably by holding R while starting a game (title takeover; the Album applet has
less memory). `woodyre.cfg`, `woodyre.sav`, `woodyre.log` and `mods/` live in `/switch/woodyre`. The buttons follow
their labels: A jumps and confirms, B ducks and goes back, X attacks, Y is the special attack, ZR / ZL as RT / LT. Joy-Cons
(attached or held as a pair) and the Pro Controller work.

**Supported versions:** the English PC CD-ROM, version 1.00 (October 2001), the Spanish CD (same game data as 1.00), and
three CDs with the February 2002 data (a few level fixes): the Brazilian Portuguese "Pica-Pau: A Fuga do Parque do Zeca
Urubu!", the Polish "Wielka Draka w Parku Buzza Buzzarda" and the Russian release by 1C. The game is in the CD's language,
and so are the port's own menus (Display, Controls).
`WoodyRE.exe --verify` checks your copy and names the release (output in `woodyre.log`); see
[docs/RELEASES.md](docs/RELEASES.md) for what differs between them. Other releases may work but are untested.

### Controls
| Action | Keys | PlayStation pad | Xbox naming |
|---|---|---|---|
| Walk | arrow keys or WASD (relative to the camera) | left stick or D-pad | left stick or D-pad |
| Jump | Space | Cross | A |
| Attack (peck, charge on release) | Left Ctrl or Shift | Square or R2 | X or RT |
| Special attack | Right Ctrl or E | Triangle or L2 | Y or LT |
| Duck | X | Circle | B |
| Look around (aim with either stick) | Enter or V | R1 | RB |
| Camera behind Woody | C or Numpad 0 | L1 or R3 | LB or RS |
| Turn / raise the camera | | right stick | right stick |
| Pause menu | Esc | Options | Start |
| Fullscreen / window | F11 | | |

Menus: Enter or Jump confirms, Esc or Duck goes back. Options → Controls changes the keys and pad buttons, and the camera
speed of the right stick (25 % to 200 %; a slow camera is gentler if you get motion sick).

Touch screens (Android): the left half of the screen is a stick wherever you put your thumb down (it walks, and moves
through the menus). On the right are the four pad buttons: A (green, jump / confirm), B (red, duck / back), X (blue,
attack) and Y (yellow, special), above them camera behind (LB) and look around (RB), and pause at the top right. The
back button or gesture works as Esc.

Controllers: a DualSense (PS5) or DualShock 4 (PS4) works over USB or Bluetooth without extra software, and so do Xbox
controllers and other XInput pads; all with rumble (Options, Vibration sets its strength). Any other joystick works as in
the original (WinMM). On Linux every pad SDL2 knows works, the Steam Deck's own controls included.

### Settings and files
Everything the game writes stays next to `WoodyRE.exe` (or in `%LOCALAPPDATA%\WoodyRE` when that folder is read-only, such
as `Program Files`, or a temporary one, such as when the exe is started from inside the zip):
- `woodyre.cfg`: the options (sound volumes, rumble strength and the Display page: resolution, window or fullscreen, 4:3
  or wide, vsync, frame cap, `logos=0` to skip the intro films, `pad_deadzone=30` for the stick dead zone in percent).
- `woodyre.sav`: the four save slots, in the original `Woody.sav` layout. An original `Woody.sav` placed next to the exe
  is imported once.
- `woodyre.log`: the engine log, useful in bug reports.

Command line: `WoodyRE.exe [--windowed | --fullscreen] [--res WxH] [--aspect 4:3|wide] [--nologo] [--verify] [--dumptex]`.

### Texture packs
Any texture can be replaced by a PNG of any size, for example an HD version. Put the PNGs in `mods\textures\` next to
`woodyre.cfg` (subfolders are fine). To make one, start the game with `--dumptex` and play: every texture it shows is
written to `mods\dump\<level>\` as `<w>x<h>_<hash>.png`. Edit or upscale a file, keep its `_<hash>.png` ending (the part
before it may be anything) and put it in `mods\textures\`. Keep the aspect ratio; transparency works through the PNG's
alpha. Details in [docs/TEXTURES.md](docs/TEXTURES.md).

## Building from source
Windows 10 or 11; nothing needs to be installed first.
```bat
build.bat
```
This builds `WoodyRE.exe` in the repository folder. The C compiler is [Zig](https://ziglang.org): an existing `zig`
or `pip install ziglang` is used when present, otherwise `build.bat` downloads the official Zig 0.16.0 for Windows
(about 95 MB) into `tools\zig` once and checks its SHA-256.

- Linux: `./build.sh` builds `woodyre` with the system's C compiler and SDL2 (Debian / Ubuntu:
  `sudo apt install build-essential libsdl2-dev`). The engine is the same; only the window, input, sound, pads
  (`src/plat_sdl.c`, `src/pad_sdl.c`) and the first-start copy (`src/datasetup_posix.c`) are SDL / POSIX code.
- Android: `cd android` then `gradlew assembleRelease` (or `assembleDebug`) builds the APK in
  `android/app/build/outputs/apk/`. It needs the Android SDK with NDK 27.2 and CMake 3.22 (Android Studio's SDK Manager
  installs both; Android Studio can also open the `android` folder directly). The build downloads SDL 2.32.10 and checks
  its SHA-256. The engine is the same as on Linux; OpenGL ES stands in for desktop OpenGL through `src/gles/`, the touch
  controls are `src/touch.c`. Without your own signing key (`-PwoodyKeystore=... -PwoodyKeyAlias=... -PwoodyKeyPassword=...`)
  the release APK is signed with the debug key, which is fine for installing it yourself.
- Nintendo Switch: `./build_switch.sh` builds `woodyre.nro` with devkitPro's devkitA64, libnx, switch-sdl2 and
  switch-mesa. Without `$DEVKITPRO` set it builds in the official `devkitpro/devkita64` Docker image instead. The same SDL
  code as on Linux and Android, on OpenGL ES through `src/gles/`; the `__SWITCH__` parts are the data folder on the SD
  card (`src/datasetup_posix.c`) and the system's error dialog for messages (`src/plat_sdl.c`).
- `build.bat dev`: the developer build `out\woody.exe`, which logs to the console and keeps the developer keys
  (F1-F5, Tab, `[ ]`, P, PgUp/PgDn, End; in `WoodyRE.exe` they need `WOODY_DEBUGKEYS=1`).
- `make_standalone.bat`: packs `WoodyRE.exe` and **your** game files from `data\` into a single
  `WoodyRE-standalone.exe` that unpacks itself to `%LOCALAPPDATA%\WoodyRE`. It contains the game's data, so it is
  for your own use only: never share or upload it.

## License
The code in this repository is licensed under the [GNU General Public License v3.0](LICENSE) or later. This does not
cover the original game, its data or its trademarks, which belong to their owners and are not part of this project.
`src/stb/` holds Sean Barrett's stb_image and stb_image_write (public domain or MIT, see the end of each file). `android/app/src/main/java/org/libsdl/` is SDL's Android code (SDL 2.32.10, zlib license); the
Android build downloads SDL itself.

# Development and reverse engineering
Developers keep the CD's contents in `extract/` and the installed original (exe, DLLs) in `game/`; both are ignored by
git, as are `data/` and the scratch folder `out/`. The original's internals are documented in [docs/](docs) (open work:
[docs/TODO.md](docs/TODO.md)).

## Status
- **Script VM ("EKO CODE") fully dissected**: file format, 63 opcodes, tick/scheduler, message routing → [docs/VM.md](docs/VM.md)
- **Disassembler** `tools/ekodisasm.py` (0 errors on all 28 levels) and **emulator** `tools/ekovm.py`
- **C implementation** `src/ekovm.c` (+ `src/ekorun.c` test harness): message traces identical to the Python emulator
- **Message catalogue** script ↔ engine → [docs/MESSAGES.md](docs/MESSAGES.md)
- **Original runs on Windows 11 and is live-traced** (`tools/wtrace.py`, custom Win32 debugger): message stream of the C VM identical to the original (House 854 ticks, W1A init+2 ticks) → [docs/TRACING.md](docs/TRACING.md)
- **RKET resource banks fully dissected** (audio, 2D images, glyph strings, font; no 3D data) → [docs/RCK.md](docs/RCK.md), `tools/rckparse.py`
- **All level formats dissected and validated on 28/28 levels (byte-exact to EOF)**: `.gel` world geometry + kd-tree → [docs/FORMAT_GEL.md](docs/FORMAT_GEL.md) (`tools/gelparse.py`); `.ins` models, skeletal animation, instances, cameras, trigger volumes → [docs/FORMAT_INS.md](docs/FORMAT_INS.md) (`tools/insparse.py`); `.tex/.col/.vis/.lit` → [docs/FORMAT_TEX_COL_VIS_LIT.md](docs/FORMAT_TEX_COL_VIS_LIT.md) (`tools/levelparse.py`)
- **glTF export + viewer**: `tools/export_gltf.py` writes one `.glb` per level (world with textures and per-polygon UV projection, all instances, Woody as a skinned mesh with animation); `viewer/index.html` (three.js) displays it. Can also be opened in Blender.
- Older overview analysis → [docs/ANALYSE.md](docs/ANALYSE.md)

## Building / running
```bash
pip install pefile capstone ziglang pillow
python -m ziglang cc -std=c99 -O2 -o out/ekorun.exe src/ekovm.c src/ekorun.c
./out/ekorun.exe extract/Data/W1A/code 60          # init + 60 ticks, print SEND trace
python tools/ekodisasm.py extract/Data out/ekoasm   # disassemble all levels
python tools/ekovm.py extract/Data                  # message statistics for all levels
```

## Native engine
Coordinate system: the level data is **right-handed** with y up (3ds Max export; instances carry a
rotation of -90° about x). Looking along +z, +x is therefore to the left. Vertex colors are R,G,B bytes with 128 = neutral (×2).
`src/level.c` (C loaders for .gel/.tex/.ins + pose evaluation like `0x43a3a0`), `src/render_gl.c` (Win32 + OpenGL 1.1),
`src/main_engine.c` (main loop: the script VM ticks every frame at 1/100 s, messages 1/4/6/1200 drive animation/visibility/type).
Track rotations are applied conjugated (see docs/FORMAT_INS.md, confirmed with `tools/wquat.py` against the original);
texture groups with flag bit 1 are blended additively.
The **results screen** after a level also runs: Woody floats in through the hub door (scripted action 0x4a
with root motion and its own camera track), the panel with the scores comes up, he cheers and then comes the question
"Do you want to save?" (docs/GAMEFLOW.md §5.1-5.2).
`src/player.c` is the player (Perso) controller ported from the original: movement, jumps, attacks, collision against the
`.gel` polygons and the press nodes of instances, the follow camera, and trigger volumes that send `eko_vol_perso_enter/in/leave`
to the script VM (docs/PERSO_*.md, docs/CAMERA.md); `--walk T` runs T seconds forward for tests.

**Visibility (`0x42a980`/`0x42ac10`, issue #9).** The kd-tree and the sectors of `.gel` (sections 5-7) and the `.vis` are now
loaded and used, as the original does. Per frame: the sector the camera is in → the `.vis` list of that sector →
frustum test on the sector boxes → only the polygons of what remains go to the card (with the frame stamp of `poly+4`, so
a face that is in multiple sectors gets drawn once). Instances outside those sectors no longer get lighting, shadow, or
a draw call; they do get posed though, because `player.c` collides against `node_world`. The same tree now also carries all geometry queries
(floor under a point, push-out against walls, sightlines) that used to walk the whole level first — per frame that happened ten to thirty
times, once per enemy added. `src/geltest.c` checks those queries on synthetic data against brute force.
`F4` disables the culling step by step if something disappears that should be there, `WOODY_PROF=1` shows per frame how many triangles
and sectors remain, and `WOODY_NOKD=1` makes the queries walk the whole level again.
```bash
build.bat dev                                    # out/woody.exe (the source list is in build.bat); the first argument is a data dir when it is a path
./out/woody.exe extract/Data                     # without a level: the three logo films (each press of Esc / Enter / Space skips one; --nologo, or logos=0 in woodyre.cfg, plays none), then the title screen (House, level 0); Enter starts, then the hub
./out/woody.exe extract/Data W1A                 # arrow keys/WASD walk (relative to camera), space jumps, Enter (or V) looks around (release toggles; arrows turn the view; the mouse only with WOODY_LOOKMOUSE=1, the original never polls it; docs/PERSO_LOOK.md), F5 free camera (then WASD + right mouse button), [ ] animation, Tab instance, F1-F4 toggles (F4 = culling)
./out/woody.exe extract/Data W1A --shot out/s.ppm 3   # screenshot after 3 s and stop
./out/woody.exe extract/Data W1A --res 1920x1080 --aspect 4:3   # display (port extras, docs/DISPLAY.md): --res WxH, --windowed / --fullscreen, --aspect 4:3|wide; WOODY_VSYNC=0/1, WOODY_FPSCAP=N; F11 = fullscreen; Options > Display saves them in woodyre.cfg (which also has reverse_stereo= / film_sound=, docs/SETUP.md; WOODY_REVSTEREO=0/1)
./out/woody.exe extract/Data WWS --prev W1A --stats 12 12 25 20 245   # results screen: back from W1A with these stats
./out/woody.exe extract/Data W1A --cam 537 -1800 -2450 0 -10   # camera: x y z yaw pitch (degrees)
python -m ziglang cc -std=c99 -O2 -o out/leveltest.exe src/level.c src/leveltest.c && ./out/leveltest.exe extract/Data   # parser test, 28 levels
python -m ziglang cc -std=c99 -O2 -o out/geltest.exe src/level.c src/geltest.c && ./out/geltest.exe    # kd-tree / .vis queries against brute force, no game data needed
python -m ziglang cc -std=c99 -O2 -o out/hnmtest.exe src/hnm.c src/hnmtest.c && ./out/hnmtest.exe extract/Logo/Eko.hnm out/eko 50 100   # HNM6 film decoder: frames as PPM + sound as WAV (docs/HNM.md)
cc -std=c99 -O1 -Isrc -Iout -o out/switchtest tools/native/switchtest.c src/level.c -lm   # peck switch (message 1042 + rem 0x458e40), no game data needed; see the header of switchtest.c
```

## Viewer
```bash
python tools/export_gltf.py --all            # out/gltf/<LVL>.glb (or a single level: export_gltf.py W1A [--anim N])
python -m http.server 8765                   # then http://localhost:8765/viewer/index.html?level=W1A
```

## Running the original
Mount the ISO (`Mount-DiskImage`), generate `Woody.cfg` with `out/mkcfg.exe` (build: see docs/TRACING.md; a cfg from the
mkcfg before 2026-09-26 has no sound, delete it and regenerate; every field and its Detect.exe control: docs/SETUP.md) and then
`python tools/wtrace.py game --seconds 120 --out out/trace/live.txt`; compare with `python tools/tracecmp.py out/trace/live.txt`.

## Analysis tools for Woody.exe
`tools/disasm.py` (annotated disassembly), `funcinfo.py` (function/callers/strings), `switchmap.py`
(switch → cases with strings/calls), `classmap.py` (SetTypeInstance → classes → message handlers),
`msgblocks.py` (all message handlers in one file), `drange.py` (disassembly per address range), `unshield5.py` (InstallShield 5 cab extractor).
