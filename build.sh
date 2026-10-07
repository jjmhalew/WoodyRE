#!/bin/sh
# build.sh - builds WoodyRE for Linux (and the Steam Deck) with SDL2: ./build.sh -> ./woodyre
# Needs a C compiler, SDL2 and OpenGL headers, e.g. on Debian / Ubuntu: sudo apt install build-essential libsdl2-dev
# (Windows: build.bat.) The game files come from your own CD; see README.md.
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
OUT=${1:-woodyre}
SRC="src/level.c src/render_gl.c src/main_engine.c src/player.c src/instance.c src/enemy.c src/boss.c src/water.c src/storm.c
     src/ekovm.c src/audio.c src/hud.c src/hnm.c src/ambient.c src/blackbox.c src/texpack.c src/gtao.c src/postfx.c
     src/plat_sdl.c src/pad_sdl.c src/datasetup_posix.c src/touch.c"
echo "Building $OUT ..."
# shellcheck disable=SC2086
GL="-lGL"
# GLES=1 ./build.sh: draw through OpenGL ES 3.0 / 2.0 like the Android and Switch builds (src/gles), to test that path on a PC
if [ "$GLES" = 1 ]; then SRC="$SRC src/gles/gles2.c"; GL="-Isrc/gles -lGLESv2"; fi
$CC -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 -Wno-format-truncation -o "$OUT" $SRC $(sdl2-config --cflags) $(sdl2-config --libs) $GL -lm
echo "Done: $OUT"
