#!/usr/bin/env bash
# Build the Android version of MHGU Recomp: the APK (SDL3 + libmain.so + libmhgugame.so).
#
# Usage:
#   ./build_android.sh                          # arm64-v8a (phones), debug-signed APK
#   ./build_android.sh --abi x86_64             # the emulator's ABI (the APK holds every ABI built)
#   ./build_android.sh --abi x86_64 --abi arm64-v8a
#   ./build_android.sh --install                # build, then adb install -r onto the running device
#   ./build_android.sh --dist 0.13.0            # release: dist/Android-MHGURecomp.<version>.apk (arm64-v8a only)
#   ./build_android.sh --clean                  # drop the ABIs' objects and libraries, then build
#   JOBS=8 ./build_android.sh                   # -j (default: nproc)
#
# Needs: the Android SDK (ANDROID_HOME or ANDROID_SDK_ROOT; else ~/Android/Sdk, /opt/android-sdk,
# /usr/lib/android-sdk) with ndk, cmake, build-tools, platform-tools and a platform; a JDK 17+ (JAVA_HOME,
# else the one javac belongs to). The first build of an ABI also builds SDL3, shaderc and FFmpeg for it
# (a few minutes). The device side (game files, env.txt, adb push) is in docs/research/android.md and
# README.md "Android".

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

log()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*"; }
die()  { printf 'build_android.sh: %s\n' "$*" >&2; exit 1; }

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit "${1:-0}"; }

# ------------------------------------------------------------------ options
ABIS=()
INSTALL=0
DIST=""
CLEAN=0
JOBS="${JOBS:-$(nproc)}"

while [ $# -gt 0 ]; do
    case "$1" in
        --abi)   [ $# -ge 2 ] || die "--abi needs a value (arm64-v8a or x86_64)"; ABIS+=("$2"); shift 2 ;;
        --install) INSTALL=1; shift ;;
        --dist)  [ $# -ge 2 ] || die "--dist needs the version (e.g. 0.13.0)"; DIST="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --jobs|-j) [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) die "unknown option '$1' (see --help)" ;;
    esac
done
[ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a)
for abi in "${ABIS[@]}"; do
    case "$abi" in arm64-v8a|x86_64) ;; *) die "unknown ABI '$abi': arm64-v8a (phones), x86_64 (emulator)" ;; esac
done
[ -n "$DIST" ] && [ ${#ABIS[@]} -gt 1 ] && warn "--dist builds the release from arm64-v8a only; the other ABIs are not packaged"

# ------------------------------------------------------------------ the SDK
find_sdk() {
    local d
    for d in "${ANDROID_SDK:-}" "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
             "$HOME/Android/Sdk" "$HOME/Android/sdk" /opt/android-sdk /usr/lib/android-sdk; do
        [ -n "$d" ] && [ -d "$d" ] && { printf '%s\n' "$d"; return 0; }
    done
    return 1
}
SDK="$(find_sdk)" || die "no Android SDK found: set ANDROID_HOME (it needs ndk, cmake, build-tools, platforms)"

[ -d "$SDK/ndk" ] || die "$SDK has no ndk: install one (sdkmanager 'ndk;<version>') or set NDK=..."
[ -d "$SDK/cmake" ] || die "$SDK has no cmake: sdkmanager 'cmake;3.22.1'"
[ -d "$SDK/build-tools" ] || die "$SDK has no build-tools: sdkmanager 'build-tools;<version>'"
[ -x "$SDK/platform-tools/adb" ] || die "$SDK has no platform-tools/adb: sdkmanager 'platform-tools'"
NDK_DIR="$(ls -d "$SDK"/ndk/*/ 2>/dev/null | sort -V | tail -1)"
[ -n "$NDK_DIR" ] && ls -d "$NDK_DIR"toolchains/llvm/prebuilt/*/bin/clang >/dev/null 2>&1 \
    || die "$SDK/ndk has no usable NDK toolchain: reinstall the NDK"
CMAKE_DIR="$(ls -d "$SDK"/cmake/*/ 2>/dev/null | sort -V | tail -1)"
[ -n "$CMAKE_DIR" ] && [ -x "$CMAKE_DIR/bin/cmake" ] || die "$SDK/cmake has no cmake binary"
BUILD_TOOLS="$(ls -d "$SDK"/build-tools/*/ 2>/dev/null | sort -V | tail -1)"
[ -n "$BUILD_TOOLS" ] && [ -x "$BUILD_TOOLS/aapt2" ] || die "$SDK/build-tools has no aapt2"

# A platform whose API is a plain integer: aapt2's --target-sdk-version wants one (android-36.1 would be "36.1").
find_platform() {
    local j v best=0 best_jar=""
    for j in "$SDK"/platforms/android-*/android.jar; do
        [ -f "$j" ] || continue
        v="${j#"$SDK"/platforms/android-}"; v="${v%/android.jar}"
        case "$v" in ''|*[!0-9]*) continue ;; esac
        if [ "$v" -gt "$best" ]; then best="$v"; best_jar="$j"; fi
    done
    [ -n "$best_jar" ] && printf '%s\n' "$best_jar"
}
ANDROID_JAR="$(find_platform)" || die "$SDK has no platforms/android-<n>: sdkmanager 'platforms;android-35'"
PLATFORM_API="$(basename "$(dirname "$ANDROID_JAR")" | sed 's/^android-//')"

# ------------------------------------------------------------------ the JDK
find_jdk() {
    local d j
    for d in "${JAVA_HOME:-}" /usr/lib/jvm/java-21-openjdk* /usr/lib/jvm/java-17-openjdk* /usr/lib/jvm/default; do
        [ -n "$d" ] && [ -x "$d/bin/javac" ] && { printf '%s\n' "$d"; return 0; }
    done
    j="$(command -v javac 2>/dev/null)" || return 1
    j="$(readlink -f "$j")"
    d="$(dirname "$(dirname "$j")")"
    [ -x "$d/bin/javac" ] && printf '%s\n' "$d"
}
JDK="$(find_jdk)" || die "no JDK found: install one (17+) or set JAVA_HOME"
JAVA_MAJOR="$("$JDK/bin/javac" -version 2>&1 | sed -n 's/^javac \([0-9][0-9]*\).*/\1/p')"
[ -n "$JAVA_MAJOR" ] && [ "$JAVA_MAJOR" -ge 17 ] || die "JDK 17+ needed (have: $("$JDK/bin/javac" -version 2>&1))"

export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"
export JAVA_HOME="$JDK"

log "SDK $SDK"
printf '   NDK         %s\n   cmake       %s\n   build-tools %s\n   platform    android-%s\n   JDK         %s (%s)\n   ABIs        %s\n   jobs        %s\n' \
    "$(basename "$NDK_DIR")" "$(basename "$CMAKE_DIR")" "$(basename "$BUILD_TOOLS")" "$PLATFORM_API" \
    "$JDK" "$("$JDK/bin/java" -version 2>&1 | head -1)" "${ABIS[*]}" "$JOBS"

# ------------------------------------------------------------------ install(1) workaround
# Some filesystems (ntfs-3g / fuseblk, e.g. a repo on an NTFS drive) let chmod(1) through but make coreutils'
# install(1) fail with "setting permissions ... Operation not permitted" - FFmpeg's `make install` then stops
# halfway. When that happens, put a small install(1) that copies and chmods on PATH for this build.
probe_dir="obj-android"
mkdir -p "$probe_dir"
printf x > "$probe_dir/.install-probe"
if install -m 644 "$probe_dir/.install-probe" "$probe_dir/.install-probe.2" 2>/dev/null; then
    :
else
    shim_dir="$probe_dir/.shim"
    mkdir -p "$shim_dir"
    cat > "$shim_dir/install" <<'SHIM'
#!/usr/bin/env bash
# install(1) replacement for filesystems where coreutils' chmod path fails: copy, then chmod(1).
set -e
mode=755; mkdirs=0; args=()
while [ $# -gt 0 ]; do
    case "$1" in
        -m) mode="$2"; shift 2 ;;
        -d) mkdirs=1; shift ;;
        -c|-p|-v|--preserve-timestamps|--verbose) shift ;;
        --) shift ;;
        -*) shift ;;
        *) args+=("$1"); shift ;;
    esac
done
if [ "$mkdirs" = 1 ]; then mkdir -p "${args[@]}"; exit 0; fi
n=${#args[@]}
[ "$n" -ge 2 ] || { echo "install shim: source and destination needed" >&2; exit 1; }
dest="${args[$((n-1))]}"
dest_dir=0
[ -d "$dest" ] && dest_dir=1
for ((i=0;i<n-1;i++)); do
    src="${args[$i]}"
    if [ "$dest_dir" = 1 ]; then
        cp -f "$src" "$dest/$(basename "$src")"; chmod "$mode" "$dest/$(basename "$src")"
    else
        cp -f "$src" "$dest"; chmod "$mode" "$dest"
    fi
done
SHIM
    chmod +x "$shim_dir/install"
    export PATH="$PWD/$shim_dir:$PATH"
    warn "install(1) cannot set permissions on this filesystem: using $shim_dir/install for the build"
fi
rm -f "$probe_dir/.install-probe" "$probe_dir/.install-probe.2"

# ------------------------------------------------------------------ build
if [ "$CLEAN" = 1 ]; then
    for abi in "${ABIS[@]}"; do
        log "clean: obj-android/$abi, build-android/lib/$abi"
        rm -rf "obj-android/$abi" "build-android/lib/$abi"
    done
    rm -f build-android/mhgurecomp.apk
fi

START=$SECONDS
for abi in "${ABIS[@]}"; do
    log "building $abi"
    make PLATFORM=android ABI="$abi" ANDROID_JAR="$ANDROID_JAR" -j"$JOBS"
done

APK=build-android/mhgurecomp.apk
[ -f "$APK" ] || die "the APK was not made: expected $APK"
printf '\n'
log "done in $((SECONDS - START))s: $APK ($(du -h "$APK" | cut -f1))"
ls -d build-android/lib/*/ 2>/dev/null | sed 's#build-android/lib/##; s#/$##' | while read -r a; do
    printf '   %s: %s\n' "$a" "$(du -h "build-android/lib/$a"/*.so 2>/dev/null | cut -f1 | tr '\n' ' ')"
done

# ------------------------------------------------------------------ after
if [ "$INSTALL" = 1 ]; then
    log "installing (adb)"
    "$SDK/platform-tools/adb" install -r "$APK"
fi
if [ -n "$DIST" ]; then
    log "release $DIST (arm64-v8a, release key)"
    make PLATFORM=android ABI=arm64-v8a ANDROID_JAR="$ANDROID_JAR" -j"$JOBS" dist VERSION="$DIST"
fi
