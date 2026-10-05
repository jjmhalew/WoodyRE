@echo off
rem build.bat - builds WoodyRE from source on Windows 10/11; nothing to install first.
rem   build.bat             WoodyRE.exe, the game (start it, it asks for the CD the first time)
rem   build.bat dev         out\woody.exe, the developer build (console log, same engine)
rem   build.bat standalone  WoodyRE-standalone.exe: WoodyRE.exe + YOUR game files in one exe, for your own use only
rem   build.bat texup       build\texup.exe, the image helper of make_hd_textures.bat
rem The C compiler is Zig (ziglang.org): a zig on PATH or "pip install ziglang" is used when there, otherwise the official
rem Windows build is downloaded once into tools\zig and checked against its SHA-256.
setlocal
cd /d "%~dp0"

set "ZIG_VER=0.16.0"
set "ZIG_SHA=68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"
set "ZIG_NAME=zig-x86_64-windows-%ZIG_VER%"
set "SRC=src\level.c src\render_gl.c src\main_engine.c src\player.c src\instance.c src\enemy.c src\boss.c src\water.c src\storm.c src\ekovm.c src\audio.c src\hud.c src\hnm.c src\ambient.c src\blackbox.c src\datasetup.c src\pad.c src\texpack.c"
set "LIBS=-lopengl32 -lgdi32 -luser32 -lwinmm -lbcrypt -lshell32 -lole32 -lhid -lsetupapi"
rem zig cc compiles for the CPU of the build machine unless told otherwise: an exe built on a new PC or on the CI runner
rem then uses AVX2/AVX-512 and dies with "illegal instruction" (0xc000001d) on older CPUs. Plain x86-64 runs everywhere.
set "CPU=-target x86_64-windows-gnu -mcpu=baseline"

call :find_zig || goto :fail

if /i "%~1"=="dev" (
    if not exist out mkdir out
    echo Building out\woody.exe ^(developer build^)...
    %ZIG% cc -std=c99 -O2 %CPU% -o out\woody.exe %SRC% res\woodyre.rc %LIBS% || goto :fail
    echo Done: out\woody.exe
    exit /b 0
)

if /i "%~1"=="texup" (
    if not exist build mkdir build
    %ZIG% cc -std=c99 -O2 %CPU% -o build\texup.exe tools\native\texup.c || goto :fail
    exit /b 0
)

echo Building WoodyRE.exe...
%ZIG% cc -std=c99 -O2 %CPU% -DWOODY_GUI -Wl,--subsystem,windows -o WoodyRE.exe %SRC% res\woodyre.rc %LIBS% || goto :fail
echo Done: WoodyRE.exe
if /i not "%~1"=="standalone" exit /b 0

rem ---- standalone: launcher stub + WoodyRE.exe + the game files of data\ (or a development extract\) ----
set "GAME="
if exist "data\Data\W1A\W1A.gel" if exist "data\Music.bf" set "GAME=data"
if not defined GAME if exist "extract\Data\W1A\W1A.gel" if exist "extract\Music.bf" set "GAME=extract"
if not defined GAME (
    echo.
    echo No game files found. Start WoodyRE.exe once first: it copies them from your CD into data\.
    goto :fail
)
if not exist build mkdir build
%ZIG% cc -std=c99 -O2 %CPU% -Wl,--subsystem,windows -o build\bundle_stub.exe tools\native\bundle.c res\woodyre.rc -luser32 -lgdi32 || goto :fail
%ZIG% cc -std=c99 -O2 %CPU% -o build\pack.exe tools\native\pack.c || goto :fail
echo Packing the game files from %GAME%\ ...
build\pack.exe build\bundle_stub.exe WoodyRE.exe %GAME% WoodyRE-standalone.exe || goto :fail
echo.
echo Done: WoodyRE-standalone.exe. It contains the game's data - keep it for yourself, do not share it.
exit /b 0

:find_zig
if exist "tools\zig\%ZIG_NAME%\zig.exe" (set ZIG="%CD%\tools\zig\%ZIG_NAME%\zig.exe" & exit /b 0)
where zig >nul 2>nul && (set "ZIG=zig" & exit /b 0)
python -m ziglang version >nul 2>nul && (set "ZIG=python -m ziglang" & exit /b 0)
echo Zig %ZIG_VER% (the C compiler, about 95 MB) is not here yet: downloading it from ziglang.org into tools\zig ...
if not exist tools\zig mkdir tools\zig
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri 'https://ziglang.org/download/%ZIG_VER%/%ZIG_NAME%.zip' -OutFile 'tools\zig\zig.zip'; if (([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes('tools\zig\zig.zip'))) -replace '-','') -ne '%ZIG_SHA%') { Remove-Item 'tools\zig\zig.zip'; throw 'SHA-256 of the download does not match' }" || exit /b 1
tar -xf tools\zig\zig.zip -C tools\zig || exit /b 1
del tools\zig\zig.zip
if not exist "tools\zig\%ZIG_NAME%\zig.exe" (echo The Zig archive did not contain %ZIG_NAME%\zig.exe & exit /b 1)
set ZIG="%CD%\tools\zig\%ZIG_NAME%\zig.exe"
exit /b 0

:fail
echo.
echo Build FAILED.
exit /b 1
