#!/usr/bin/env bash
# Build MHGU Recomp for Windows, then test it.
#
# Run this in one of two places:
#   * Linux (or WSL), using the UCRT64 cross toolchain in build_dep/ (see below);
#   * an MSYS2 UCRT64 shell on Windows.
#
# Either way it builds into build_windows/ and keeps the compiler's files in
# obj_windows/, leaving a native Linux build in build/ and obj/ alone.
#
# Usage:
#   ./build_windows.sh                    # uses ./mhgu (must hold exefs/ and romfs/)
#   ./build_windows.sh /path/to/game      # another folder with exefs/ and romfs/
#   JOBS=8 RUN_SECS=60 ./build_windows.sh # -j and smoke-test duration
#   SKIP_TESTS=1 ./build_windows.sh       # skip make test-lift
#   RUN_SECS=0 ./build_windows.sh         # skip the smoke test
#
# The Linux cross toolchain in build_dep/ is installed with MSYS2's UCRT64
# packages (clang/lld, gcc's C++ headers and libraries, SDL3, shaderc, Vulkan)
# plus small wrappers in build_dep/ucrt64/bin. On Windows, install MSYS2 with
# the UCRT64 packages from README.md ("Tools (Windows)") instead; set
# MSYS2_ROOT=<folder holding ucrt64\ and usr\> if MSYS2 is not at C:\msys64.
#
# What it does:
#   0. checks the game folder, the platform and the toolchain
#   1. makes tools/venv only if the recompiler's Python deps are missing
#   2. fetches vk_mem_alloc.h (3.4.0) if third_party/ doesn't have it yet
#   3. make gen       - recompile the game's ARM code to C in generated/
#   4. make -jN       - build build_windows/mhgurecomp.exe, mhgugame.dll, data, licenses
#   5. make test-lift - differential test of the recompiler against Unicorn
#   6. checks every expected build output
#   7. smoke test: runs build_windows/mhgurecomp.exe for RUN_SECS seconds and
#      checks log.txt: it passes when the game booted and no FATAL happened
#      (exiting on its own, window closed, is normal too). On Linux it runs
#      under Wine, so it is skipped unless RUN_SECS is set explicitly.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

GAME="${1:-mhgu}"
SKIP_TESTS="${SKIP_TESTS:-0}"
B="${B:-build_windows}"
O="${O:-obj_windows}"
DEP="${BUILD_DEP:-$ROOT/build_dep}"
SYSROOT="$DEP/ucrt64"
TOOLBIN="$SYSROOT/bin"

log() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { printf 'build_windows.sh: %s\n' "$*" >&2; exit 1; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST=windows ;;
    Linux*|Darwin*)      HOST=linux ;;
    *) die "unsupported host: $(uname -s)" ;;
esac

JOBS="${JOBS:-${NUMBER_OF_PROCESSORS:-}}"
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [ "$JOBS" -lt 1 ]; then
    JOBS="$(nproc 2>/dev/null || echo 8)"
fi
# A Linux cross build cannot be smoke-tested like the real thing; make that opt-in.
if [ -z "${RUN_SECS+x}" ]; then
    if [ "$HOST" = windows ]; then RUN_SECS=30; else RUN_SECS=0; fi
fi

MAKE_ARGS=(GAME="$GAME" B="$B" O="$O" PLATFORM=windows)

# ---------------------------------------------------------------------------
# 0. checks
# ---------------------------------------------------------------------------
log '0. checks'

[ -f "$GAME/exefs/main" ] || die "no exefs/main in $GAME (usage: ./build_windows.sh [folder with exefs/ and romfs/])"
[ -d "$GAME/romfs" ]      || die "no romfs/ in $GAME"

if [ "$HOST" = windows ]; then
    # If the shell's PATH does not have the UCRT64 toolchain yet, add MSYS2's.
    if ! command -v clang >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
        for d in /ucrt64/bin /mingw64/bin; do
            if [ -d "$d" ]; then
                PATH="$d:/usr/bin:$PATH"
                break
            fi
        done
    fi
    if [ -n "${MSYS2_ROOT:-}" ]; then
        if [ ! -d "$MSYS2_ROOT" ] && command -v cygpath >/dev/null 2>&1; then
            MSYS2_ROOT="$(cygpath -u "$MSYS2_ROOT")"
        fi
        for d in "$MSYS2_ROOT/ucrt64/bin" "$MSYS2_ROOT/usr/bin"; do
            [ -d "$d" ] && PATH="$d:$PATH"
        done
        export PATH
    fi

    # Accept a Windows-style game path (C:\games\mhgu or C:/games/mhgu) in bash.
    if command -v cygpath >/dev/null 2>&1 && [[ "$GAME" == *:* ]]; then
        GAME="$(cygpath -u "$GAME")"
    fi

    for c in clang clang++ lld make sh curl; do
        command -v "$c" >/dev/null || die "missing tool: $c - install the MSYS2 UCRT64 packages from README.md"
    done

    HAVEPY=()
    if py -3 -V >/dev/null 2>&1; then
        HAVEPY=(py -3)
    elif python -V >/dev/null 2>&1; then
        HAVEPY=(python)
    elif python3 -V >/dev/null 2>&1; then
        HAVEPY=(python3)
    else
        die 'no Python found - install Python for Windows from python.org'
    fi
else
    [ -x "$TOOLBIN/clang" ] || die "no cross toolchain in $DEP - install the UCRT64 packages there first"
    [ -f "$SYSROOT/include/windows.h" ] || die "$SYSROOT is not a UCRT64 tree"
    for c in /usr/bin/clang /usr/bin/clang++ /usr/bin/lld make sh curl; do
        command -v "$c" >/dev/null || die "missing tool: $c"
    done
    # build_dep/bin has the cygpath shim the Makefile's bundle step calls;
    # build_dep/ucrt64/bin also has the clang wrapper and the Windows DLLs.
    PATH="$DEP/bin:$TOOLBIN:$PATH"
    export PATH

    if command -v python3 >/dev/null 2>&1; then
        HAVEPY=(python3)
    else
        die 'no python3 found'
    fi

    MAKE_ARGS+=(CC="$TOOLBIN/clang" CXX="$TOOLBIN/clang++")
    # The bundled FFmpeg is built from source too: point its configure at the
    # cross compiler instead of letting it build for the Linux host.
    FFMPEG_CONF_CROSS='--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect'
    FFMPEG_CONF_CROSS+=' --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample'
    FFMPEG_CONF_CROSS+=' --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov'
    FFMPEG_CONF_CROSS+=' --enable-parser=h264,aac --enable-decoder=h264,aac'
    FFMPEG_CONF_CROSS+=' --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix='
    FFMPEG_CONF_CROSS+=' --ar=/usr/bin/llvm-ar --nm=/usr/bin/llvm-nm --ranlib=/usr/bin/llvm-ranlib'
    FFMPEG_CONF_CROSS+=' --strip=/usr/bin/llvm-strip --windres=/usr/bin/llvm-windres'
    MAKE_ARGS+=(FFMPEG_CONF="$FFMPEG_CONF_CROSS")

    # GNU install(1) sets permissions with fchmod, which some FUSE/NTFS mounts
    # refuse (the bundled FFmpeg runs `make install`). Build FFmpeg on the
    # host's native filesystem when the build folder has that problem.
    mkdir -p "$O"
    : > "$O/.install-probe.src"
    if ! install -m 644 "$O/.install-probe.src" "$O/.install-probe" 2>/dev/null; then
        FFMPEG_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/mhgu-recomp/ffmpeg-9.0.2"
        echo "note: $O does not allow install(1) permissions; building FFmpeg in $FFMPEG_CACHE"
        MAKE_ARGS+=(FFMPEG_DIR="$FFMPEG_CACHE")
    fi
    rm -f "$O/.install-probe.src" "$O/.install-probe"
fi

echo "game files: $GAME    jobs: -j$JOBS    output: $B/    objects: $O/"
[ "$HOST" = linux ] && echo "toolchain: $SYSROOT (cross, native clang)"

# ---------------------------------------------------------------------------
# 1. Python venv (the Makefile uses tools/venv when it exists)
# ---------------------------------------------------------------------------
if [ ! -x tools/venv/Scripts/python.exe ] && [ ! -x tools/venv/bin/python ]; then
    log '1. python venv (recompiler deps: lz4, capstone, unicorn)'
    "${HAVEPY[@]}" -m venv tools/venv
    if [ -x tools/venv/Scripts/pip.exe ]; then
        PIP=tools/venv/Scripts/pip.exe
    elif [ -x tools/venv/Scripts/pip ]; then
        PIP=tools/venv/Scripts/pip
    elif [ -x tools/venv/bin/pip ]; then
        PIP=tools/venv/bin/pip
    else
        die 'venv creation failed: no pip in tools/venv'
    fi
    "$PIP" install -r tools/requirements.txt || die 'pip failed to install tools/requirements.txt'
fi

# ---------------------------------------------------------------------------
# 2. vk_mem_alloc.h
# ---------------------------------------------------------------------------
if [ ! -s third_party/vk_mem_alloc/vk_mem_alloc.h ]; then
    log '2. fetch vk_mem_alloc.h 3.4.0'
    mkdir -p third_party/vk_mem_alloc
    curl -fL --retry 3 -o third_party/vk_mem_alloc/vk_mem_alloc.h \
        https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h \
        || die 'fetching vk_mem_alloc.h failed'
fi
grep -q 'Vulkan Memory Allocator' third_party/vk_mem_alloc/vk_mem_alloc.h \
    || die 'third_party/vk_mem_alloc/vk_mem_alloc.h looks wrong'

# ---------------------------------------------------------------------------
# 3. recompile the game code (generated/ is shared with other builds)
# ---------------------------------------------------------------------------
log '3. recompile the game code (make gen)'
make gen "${MAKE_ARGS[@]}"

# ---------------------------------------------------------------------------
# 4. build everything
# ---------------------------------------------------------------------------
log "4. build everything (make -j$JOBS)"
make -j"$JOBS" "${MAKE_ARGS[@]}" || die 'build failed'

# ---------------------------------------------------------------------------
# 5. differential test of the recompiler
# ---------------------------------------------------------------------------
if [ "$SKIP_TESTS" != 1 ]; then
    log '5. differential test of the recompiler (make test-lift)'
    make test-lift "${MAKE_ARGS[@]}"
else
    log '5. skipping make test-lift (SKIP_TESTS=1)'
fi

# ---------------------------------------------------------------------------
# 6. build outputs
# ---------------------------------------------------------------------------
log '6. build outputs'
missing=0
for f in "$B/mhgurecomp.exe" "$B/mhgugame.dll" "$B/data/exefs/main" "$B/data/romfs" \
         "$B/licenses/THIRD_PARTY_NOTICES.txt" "$B/licenses/LICENSE.txt"; do
    if [ -e "$f" ]; then
        echo "  ok    $f"
    else
        echo "  MISS  $f"
        missing=1
    fi
done
[ "$missing" = 0 ] || die 'the build did not produce all the expected files'

# ---------------------------------------------------------------------------
# 7. smoke test
# ---------------------------------------------------------------------------
if [ "$RUN_SECS" -gt 0 ]; then
    if [ "$HOST" = windows ]; then
        log "7. smoke test: run $B/mhgurecomp.exe for ${RUN_SECS}s (a game window opens)"
        rm -f "$B/log.txt"
        ( cd "$B" && ./mhgurecomp.exe >/dev/null 2>log.txt ) &
        sleep "$RUN_SECS"
        if tasklist //FI 'IMAGENAME eq mhgurecomp.exe' 2>/dev/null | grep -qi 'mhgurecomp.exe'; then
            taskkill //F //IM mhgurecomp.exe >/dev/null 2>&1 || true
            echo "  ok    game survived the full ${RUN_SECS}s and was stopped"
        else
            echo '  ok    game exited on its own (window closed, normal)'
        fi
    else
        log "7. smoke test: run $B/mhgurecomp.exe under Wine for ${RUN_SECS}s"
        rm -f "$B/log.txt"
        ( cd "$B" && wine ./mhgurecomp.exe >/dev/null 2>log.txt ) &
        pid=$!
        sleep "$RUN_SECS"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wineserver -k 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            echo "  ok    game survived the full ${RUN_SECS}s and was stopped"
        else
            echo '  ok    game exited on its own (window closed, normal)'
        fi
    fi

    if ! grep -q 'main thread: prio' "$B/log.txt" 2>/dev/null; then
        tail -30 "$B/log.txt" 2>/dev/null || true
        die "smoke test failed: $B/log.txt never reached 'main thread: prio'"
    fi
    if grep -q 'FATAL' "$B/log.txt" 2>/dev/null; then
        tail -30 "$B/log.txt" 2>/dev/null || true
        die "smoke test failed: $B/log.txt holds a FATAL"
    fi
    echo "  ok    log.txt reached 'main thread: prio', no FATAL"
else
    log '7. skipping the smoke test (RUN_SECS=0; set RUN_SECS=30 to run it)'
fi

log 'done'
echo "Everything built and tested. To play:"
echo "  cd $B && ./mhgurecomp.exe            # settings, caches, mods, log beside it"
echo "  MHGU_HOME=/c/myfolder ./mhgurecomp.exe  # ...or keep those in another folder"
echo "Saves: %APPDATA%\\MHGURecomp\\save."
