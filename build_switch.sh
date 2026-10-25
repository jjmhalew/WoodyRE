#!/bin/sh
# build_switch.sh - builds WoodyRE for a Nintendo Switch with homebrew (custom firmware such as Atmosphere):
#   ./build_switch.sh -> woodyre.nro; copy it to /switch/woodyre/ on the SD card and start it from the Homebrew Menu.
# Needs devkitPro's devkitA64 with switch-sdl2 and switch-mesa (devkitpro.org). Without $DEVKITPRO it builds in the official
# devkitpro/devkita64 Docker image instead, which has all of that. The game files come from your own CD; see README.md.
set -e
cd "$(dirname "$0")"
if [ -z "$DEVKITPRO" ]; then
    echo "DEVKITPRO is not set: building in the devkitpro/devkita64 Docker image ..."
    exec docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 sh ./build_switch.sh "$@"
fi
OUT=${1:-woodyre}
VERSION=$(sed -n 's/^FILEVERSION *\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1.\2.\3/p' res/woodyre.rc)
PORT=$DEVKITPRO/portlibs/switch
PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
SRC="src/level.c src/render_gl.c src/main_engine.c src/player.c src/instance.c src/enemy.c src/boss.c src/water.c src/storm.c
     src/ekovm.c src/audio.c src/hud.c src/hnm.c src/ambient.c src/blackbox.c src/texpack.c src/gtao.c src/postfx.c
     src/plat_sdl.c src/pad_sdl.c src/datasetup_posix.c src/touch.c src/gles/gles2.c"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
# src/gles first on the include path: its GL/gl.h puts the engine's fixed-function OpenGL on OpenGL ES 2 / 3 (as on Android);
# char is unsigned on ARM, the engine is written for x86, where it is signed
CFLAGS="-std=gnu99 -O2 -ffunction-sections -fsigned-char -Wno-format-truncation $ARCH -D__SWITCH__ -DWOODY_GUI
        -Isrc/gles -Isrc -I$PORT/include -I$PORT/include/SDL2 -I$DEVKITPRO/libnx/include"
LIBS="-L$PORT/lib -L$DEVKITPRO/libnx/lib -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lstdc++ -lnx -lm"
mkdir -p build/switch
echo "Building $OUT.nro (WoodyRE $VERSION) ..."
# shellcheck disable=SC2086
aarch64-none-elf-gcc $CFLAGS -specs="$DEVKITPRO/libnx/switch.specs" -Wl,-Map,build/switch/$OUT.map -o build/switch/$OUT.elf $SRC $LIBS
nacptool --create "WoodyRE" "jjmhalew" "$VERSION" build/switch/$OUT.nacp
elf2nro build/switch/$OUT.elf "$OUT.nro" --icon=res/switch_icon.jpg --nacp=build/switch/$OUT.nacp
echo "Done: $OUT.nro"
