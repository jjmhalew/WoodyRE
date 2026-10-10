@echo off
rem make_android_bundle.bat [WoodyRE.apk] [game.iso] - one APK with everything inside (WoodyRE-bundle.apk): the Android app
rem + YOUR ISO image of the CD. At its first start the app unpacks the image without asking for it. Drag the APK and the ISO
rem onto this file, or put them next to it (the newest WoodyRE-*.apk, else the one android\ built, and any *.iso are taken).
rem Because it contains the game's data it is for your own devices only: never share or upload it.
rem It is signed with a key of your own (made once, kept in %LOCALAPPDATA%\WoodyRE\android-bundle.key): later bundles install
rem as updates of it, but not over a WoodyRE from the Releases page - uninstall that first (its saves go with it).
setlocal
cd /d "%~dp0"
set "APK="
set "ISO="
for %%a in (%*) do if /i "%%~xa"==".apk" (set "APK=%%~fa") else (set "ISO=%%~fa")
if not defined APK for /f "delims=" %%f in ('dir /b /o:d WoodyRE-*.apk 2^>nul ^| findstr /v /i /l /x "WoodyRE-bundle.apk"') do set "APK=%CD%\%%f"
if not defined APK if exist "android\app\build\outputs\apk\release\app-release.apk" set "APK=%CD%\android\app\build\outputs\apk\release\app-release.apk"
if not defined ISO for %%f in (*.iso) do set "ISO=%%~ff"
if not defined APK (
    echo No APK: drag WoodyRE-^<version^>.apk from the Releases page onto this file, or put it next to it.
    goto :fail
)
if not defined ISO (
    echo No ISO image of the CD: drag it onto this file, or put it next to it.
    goto :fail
)
call "%~dp0build.bat" apkbundle || goto :fail
echo APK: %APK%
echo ISO: %ISO%
build\apkbundle.exe "%APK%" "%ISO%" WoodyRE-bundle.apk game.iso "%LOCALAPPDATA%\WoodyRE\android-bundle.key" "WoodyRE bundle" || goto :fail
echo.
echo Done: WoodyRE-bundle.apk. Install it on your phone or tablet (a USB cable, or adb install WoodyRE-bundle.apk).
echo It contains the game's data - keep it for yourself, do not share it.
pause
exit /b 0

:fail
echo.
echo No bundle made.
pause
exit /b 1
