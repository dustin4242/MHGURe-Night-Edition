#!/usr/bin/env bash
# Build MHGU Recomp on Linux, then test it.
#
# Usage:
#   ./build.sh                    # uses ./mhgu (must hold exefs/ and romfs/)
#   ./build.sh /path/to/game      # another folder with exefs/ and romfs/
#   JOBS=8 RUN_SECS=60 ./build.sh # -j and smoke-test duration
#
# What it does:
#   0. checks the game folder and the toolchain
#   1. makes tools/venv only if the recompiler's Python deps are missing
#   2. fetches vk_mem_alloc.h (3.4.0) if third_party/ doesn't have it yet
#   3. make gen       - recompile the game's ARM code to C in generated/
#   4. make -jN       - build build/mhgurecomp, build/mhgugame.so, build/data, licenses
#   5. make test-lift - differential test of the recompiler against Unicorn
#   6. checks every expected build output
#   7. smoke test: runs build/mhgurecomp for RUN_SECS seconds; it passes if the
#      game booted and was still alive when the timeout killed it (checks log.txt)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

GAME="${1:-mhgu}"
JOBS="${JOBS:-$(nproc)}"
RUN_SECS="${RUN_SECS:-30}"

log() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*"; exit 1; }

log '0. checks'
[ -f "$GAME/exefs/main" ] || die "no exefs/main in $GAME (usage: ./build.sh [folder with exefs/ and romfs/])"
[ -d "$GAME/romfs" ]      || die "no romfs/ in $GAME"
for c in clang clang++ lld make python3 curl timeout; do
    command -v "$c" >/dev/null || die "missing tool: $c"
done
echo "game files: $GAME    jobs: -j$JOBS"

if [ ! -s third_party/vk_mem_alloc/vk_mem_alloc.h ]; then
    log '2. fetch vk_mem_alloc.h 3.4.0'
    mkdir -p third_party/vk_mem_alloc
    curl -fL --retry 3 -o third_party/vk_mem_alloc/vk_mem_alloc.h \
        https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
fi
grep -q 'Vulkan Memory Allocator' third_party/vk_mem_alloc/vk_mem_alloc.h \
    || die 'third_party/vk_mem_alloc/vk_mem_alloc.h looks wrong'

# the Makefile runs the tools as `cd tools && ../tools/venv/bin/python ...`, so the venv is needed
# on Linux even when the system python has the same packages
if [ ! -d tools/venv ]; then
    log '1. python venv (recompiler deps: lz4, capstone, unicorn)'
    python3 -m venv tools/venv
    tools/venv/bin/pip install -r tools/requirements.txt
fi

log '3. recompile the game code (make gen)'
make gen GAME="$GAME"

log '4. build everything (make -j'"$JOBS"')'
if ! make -j"$JOBS" GAME="$GAME"; then
    # the bundled FFmpeg runs `make install`, whose chmod can fail on FUSE / encrypted mounts.
    # Fall back to the system's libavformat / libavcodec / libavutil (FFMPEG=system, FFmpeg 5.1+).
    have_sys=1
    for pkg in libavformat libavcodec libavutil; do
        pkg-config --exists "$pkg" || have_sys=0
    done
    [ -f /usr/include/libavformat/avformat.h ] || have_sys=0
    if [ "$have_sys" = 1 ]; then
        log '4b. retry with FFMPEG=system (system libav* instead of the bundled build)'
        rm -f obj/rt/hle/movie.o   # recompile it against the system headers, not the bundled ones
        make -j"$JOBS" GAME="$GAME" FFMPEG=system || die 'build failed, also with FFMPEG=system'
    else
        die 'build failed and there is no system FFmpeg to fall back to'
    fi
fi

log '5. differential test of the recompiler (make test-lift)'
make test-lift GAME="$GAME"

log '6. build outputs'
missing=0
for f in build/mhgurecomp build/mhgugame.so build/data/exefs/main build/data/romfs \
         build/licenses/THIRD_PARTY_NOTICES.txt build/licenses/LICENSE.txt; do
    if [ -e "$f" ]; then
        echo "  ok    $f"
    else
        echo "  MISS  $f"
        missing=1
    fi
done
[ "$missing" = 0 ] || die 'the build did not produce all the expected files'

log "7. smoke test: run build/mhgurecomp for ${RUN_SECS}s (a game window opens)"
rm -f build/log.txt
t0=$(date +%s)
rc=0
( cd build && timeout -k 5 "$RUN_SECS" ./mhgurecomp >/dev/null 2>&1 ) || rc=$?
elapsed=$(( $(date +%s) - t0 ))
# pass = it reached the game's main thread, never hit a FATAL, and either survived
# the timeout (exit 124) or exited on its own with 0 (a closed window is normal)
if grep -q 'main thread: prio' build/log.txt 2>/dev/null && ! grep -q 'FATAL' build/log.txt 2>/dev/null \
   && { [ "$rc" = 0 ] || [ "$rc" = 124 ]; }; then
    if [ "$rc" = 0 ]; then
        echo "  ok    game booted, ran ${elapsed}s, then exited on its own (window closed, normal)"
    else
        echo "  ok    game booted and survived the full ${elapsed}s timeout (exit $rc)"
    fi
    echo "  ok    log.txt reached 'main thread: prio', no FATAL"
else
    echo "  FAIL  smoke test: exit $rc after ${elapsed}s"
    tail -30 build/log.txt 2>/dev/null || echo 'no log.txt written'
    die 'smoke test failed: check the tail of build/log.txt above'
fi

log 'done'
echo "Everything built and tested. To play:"
echo "  cd build && ./mhgurecomp            # settings, caches, mods, log beside it"
echo "  MHGU_HOME=~/mygame ./mhgurecomp     # ...or keep those in another folder"
echo "Saves: \$XDG_DATA_HOME/MHGURecomp/save (~/.local/share/MHGURecomp/save)."
