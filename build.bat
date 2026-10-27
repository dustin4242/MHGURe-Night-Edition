@echo off
rem ========================================================================
rem build.bat - build MHGU Recomp on Windows, then test it: the Windows twin
rem of build.sh. Run this from a Command Prompt in this folder.
rem
rem Needs MSYS2 with the UCRT64 packages from README.md at C:\msys64
rem (override that with MSYS2_ROOT=<folder holding ucrt64\ and usr\>) and
rem Python for Windows (python.org) for the recompiler's tools.
rem
rem Usage:
rem   build.bat                       # build from .\mhgu (must hold exefs\ and romfs\)
rem   build.bat C:\path\to\game       # another folder holding exefs\ and romfs\
rem   set JOBS=8 & set RUN_SECS=60 & build.bat   # -j and smoke-test duration
rem
rem What it does:
rem   0. checks the game folder and the MSYS2 toolchain
rem   1. makes tools\venv only if the recompiler's Python deps are missing
rem   2. fetches vk_mem_alloc.h (3.4.0) if third_party\ doesn't have it yet
rem   3. make gen       - recompile the game's ARM code to C in generated\
rem   4. make -jN       - build build\mhgurecomp.exe, mhgugame.dll, data, licenses
rem   5. make test-lift - differential test of the recompiler against Unicorn
rem   6. checks every expected build output
rem   7. smoke test: runs build\mhgurecomp.exe detached for RUN_SECS seconds,
rem      kills it, and checks log.txt: it passes when the game booted and no
rem      FATAL happened (exiting on its own, window closed, is normal too)
rem ========================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "GAME=%~1"
if not defined GAME set "GAME=mhgu"
set "B=build"
set "O=obj"
if not defined RUN_SECS set "RUN_SECS=30"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MB=%MSYS2_ROOT%\ucrt64\bin"
set "MU=%MSYS2_ROOT%\usr\bin"
if not defined JOBS (
    set "JOBS=%NUMBER_OF_PROCESSORS%"
    if "!JOBS:~0,1!"=="%" set "JOBS=8"
)
rem make's recipes run in sh and need the MSYS2 tools in PATH
set "PATH=%MB%;%MU%;%PATH%"

call :log 0. checks
if not exist "%GAME%\exefs\main" (
    echo build.bat: no exefs\main in %GAME%
    echo            usage: build.bat [folder holding exefs\ and romfs\]
    exit /b 1
)
if not exist "%GAME%\romfs" (
    echo build.bat: no romfs\ in %GAME%
    exit /b 1
)

set "MISSING="
for %%t in (make clang clang++ lld sh curl) do (
    %%t --version >nul 2>&1
    if errorlevel 1 set "MISSING=!MISSING! %%t"
)
if defined MISSING (
    echo build.bat: MSYS2 is missing: !MISSING! - install the UCRT64 packages from
    echo the README.md "Tools" section for Windows first; set MSYS2_ROOT if your
    echo MSYS2 is not at C:\msys64
    exit /b 1
)
set "HAVEPY="
py -3 -V >nul 2>&1 && set "HAVEPY=py -3"
python -V >nul 2>&1 && set "HAVEPY=python"
if not defined HAVEPY (
    echo build.bat: no Python found - install Python for Windows, from python.org
    exit /b 1
)
echo game files: %GAME%    jobs: -j%JOBS%    MSYS2: %MSYS2_ROOT%

if not exist "tools\venv\Scripts\python.exe" (
    call :log 1. python venv, recompiler deps: lz4, capstone, unicorn
    !HAVEPY! -m venv tools\venv
    if not exist "tools\venv\Scripts\python.exe" (
        echo build.bat: venv creation failed
        exit /b 1
    )
    tools\venv\Scripts\pip install -r tools\requirements.txt || (
        echo build.bat: pip failed to install tools\requirements.txt
        exit /b 1
    )
)

call :log 2. vk_mem_alloc.h 3.4.0
if not exist "third_party\vk_mem_alloc\vk_mem_alloc.h" (
    mkdir "third_party\vk_mem_alloc" 2>nul
    curl -fL --retry 3 -o "third_party\vk_mem_alloc\vk_mem_alloc.h" https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
    if errorlevel 1 (
        echo build.bat: fetching vk_mem_alloc.h failed
        exit /b 1
    )
)
findstr /i "Vulkan Allocator" "third_party\vk_mem_alloc\vk_mem_alloc.h" >nul 2>&1
if errorlevel 1 (
    echo build.bat: third_party\vk_mem_alloc\vk_mem_alloc.h is missing or wrong
    exit /b 1
)

call :log 3. recompile the game code, make gen
call make gen GAME="%GAME%"
if errorlevel 1 exit /b 1

call :log 4. build everything, make -j%JOBS%
call make -j%JOBS% GAME="%GAME%"
if errorlevel 1 exit /b 1

call :log 5. differential test of the recompiler, make test-lift
call make test-lift GAME="%GAME%"
if errorlevel 1 exit /b 1

call :log 6. build outputs
set "MISSING=0"
for %%f in ("%B%\mhgurecomp.exe" "%B%\mhgugame.dll" "%B%\data\exefs" "%B%\data\romfs" "%B%\licenses\THIRD_PARTY_NOTICES.txt" "%B%\licenses\LICENSE.txt") do (
    if exist %%f (echo   ok    %%f) else (echo   MISS  %%f & set "MISSING=1")
)
if %MISSING% equ 1 (
    echo build.bat: the build did not produce all the expected files
    exit /b 1
)

call :log 7. smoke test: run build\mhgurecomp.exe for ~%RUN_SECS% s, a game window opens
pushd "%B%"
del "log.txt" 2>nul
start "" /b cmd /c "mhgurecomp.exe 2> log.txt"
call :sleep %RUN_SECS%
taskkill /f /im mhgurecomp.exe >nul 2>&1
if errorlevel 1 (echo   note  game already exited on its own, window closed: normal) else (echo   ok    game survived the full ~%RUN_SECS% s and was killed)
popd
set "PAT=%O%\smoke_pat.txt"
>"%PAT%" echo main thread: prio
findstr /g:"%PAT%" "%B%\log.txt" >nul 2>&1
if errorlevel 1 (
    echo   FAIL  log.txt never reached 'main thread: prio', check build\log.txt
    exit /b 1
)
findstr /i fatal "%B%\log.txt" >nul 2>&1
if not errorlevel 1 (
    echo   FAIL  log.txt holds a FATAL, check build\log.txt
    exit /b 1
)
echo   ok    log.txt reached 'main thread: prio', no FATAL

call :log done
echo Everything built and tested. To play:
echo   run-game.bat                   # settings, caches, mods\ and the log go into run\
echo   cd build ^&^& mhgurecomp.exe   # ...or start it here and let those go into build\
echo Saves: %APPDATA%\MHGURecomp\save.
exit /b 0

:log
echo.
echo == %*
exit /b 0

:sleep
rem ~%1 seconds: ping counts one second per try (sleep(1) has no twin here)
set /a "N=%1+1"
ping -n %N% -w 1000 127.0.0.1 >nul 2>&1
exit /b 0
