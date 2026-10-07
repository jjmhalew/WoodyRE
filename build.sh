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
$CC -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 -Wno-format-truncation -o "$OUT" $SRC $(sdl2-config --cflags) $(sdl2-config --libs) -lGL -lm
echo "Done: $OUT"
