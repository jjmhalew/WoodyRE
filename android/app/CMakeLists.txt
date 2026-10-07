# The native part of the Android build: SDL2 (fetched, the release whose Java glue is in src/main/java/org/libsdl) and the
# engine of ../../src as libmain.so, on OpenGL ES 2.0 / 3.0 through ../../src/gles (GL/gl.h there replaces the desktop header).
cmake_minimum_required(VERSION 3.18)
project(WoodyRE C)

include(FetchContent)
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
add_library(main SHARED
    ${S}/level.c ${S}/render_gl.c ${S}/main_engine.c ${S}/player.c ${S}/instance.c ${S}/enemy.c ${S}/boss.c ${S}/water.c
    ${S}/storm.c ${S}/ekovm.c ${S}/audio.c ${S}/hud.c ${S}/hnm.c ${S}/ambient.c ${S}/blackbox.c ${S}/texpack.c ${S}/gtao.c ${S}/postfx.c
    ${S}/plat_sdl.c ${S}/pad_sdl.c ${S}/datasetup_posix.c ${S}/touch.c ${S}/gles/gles2.c)
target_include_directories(main PRIVATE ${S}/gles ${S})
target_compile_definitions(main PRIVATE WOODY_GUI)                  # the release build: stdout to woodyre.log beside the data
# char is unsigned on ARM; the engine is written for x86, where it is signed
target_compile_options(main PRIVATE -std=gnu99 -O2 -fsigned-char -Wno-format-truncation)
set_source_files_properties(${S}/main_engine.c PROPERTIES COMPILE_DEFINITIONS main=SDL_main)   # SDLActivity calls SDL_main
target_link_libraries(main PRIVATE SDL2::SDL2 GLESv2 log android m)
