#!/usr/bin/env bash
# Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md
# ---------------------------------------------------------------------------
# SpaceSync on Linux: build, stage and (optionally) install.
#
#   ./build.sh             configure (if needed) + build + stage into out/linux/SpaceSync
#   ./build.sh clean       delete the build directory first, then do the above
#   ./build.sh install     build + stage, then copy to $SPACESYNC_HOME
#                          (default ~/.local/share/spacesync) and register with SteamVR
#   ./build.sh uninstall   unregister from SteamVR and remove $SPACESYNC_HOME
#
# Requirements: cmake, ninja, a C++20 compiler, Vulkan headers/loader.
# install/uninstall do what the Windows installer does: register the driver
# with vrpathreg, register the overlay manifest (autolaunch on) and turn on
# activateMultipleDrivers. SteamVR must be closed for them.
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/out/build/linux-release"
STAGE="$ROOT/out/linux/SpaceSync"
INSTALL_DIR="${SPACESYNC_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/spacesync}"

DO_CLEAN=0
ACTION=build
for arg in "$@"; do
    case "$arg" in
        clean) DO_CLEAN=1 ;;
        install) ACTION=install ;;
        uninstall) ACTION=uninstall ;;
        *) echo "Usage: $0 [clean] [install|uninstall]" >&2; exit 2 ;;
    esac
done

fail() { echo "[build] $*" >&2; exit 1; }

steamvr_running() { pgrep -x vrserver >/dev/null 2>&1; }

# vrpathreg.sh from the SteamVR runtime that OpenVR points at.
vrpathreg() {
    local runtime
    runtime="$("$1/SpaceSync" -openvrpath 2>/dev/null)" || fail "SteamVR runtime not found. Install SteamVR and start it once."
    [[ -x "$runtime/bin/vrpathreg.sh" ]] || fail "vrpathreg.sh not found in $runtime/bin"
    echo "$runtime/bin/vrpathreg.sh"
}

if [[ "$ACTION" == uninstall ]]; then
    steamvr_running && fail "Close SteamVR first."
    [[ -x "$INSTALL_DIR/SpaceSync" ]] || fail "No install found in $INSTALL_DIR"
    "$(vrpathreg "$INSTALL_DIR")" removedriver "$INSTALL_DIR/driver" || true
    "$INSTALL_DIR/SpaceSync" -removemanifest || true
    rm -rf -- "$INSTALL_DIR"
    echo "[build] Uninstalled from $INSTALL_DIR"
    exit 0
fi

for cmd in cmake ninja git; do
    command -v "$cmd" >/dev/null || fail "Missing required command: $cmd"
done

cd "$ROOT"
if [[ ! -f 3rdparty/OpenVR/headers/openvr.h ]]; then
    echo "[build] Submodules missing, fetching ..."
    git submodule update --init --recursive
fi

if (( DO_CLEAN )); then
    echo "[build] Cleaning $BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "[build] Configuring ..."
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi

echo "[build] Building ..."
cmake --build "$BUILD_DIR"

# Same layout the Windows installer produces: app files at the top,
# the SteamVR driver in driver/.
echo "[build] Staging into $STAGE ..."
rm -rf -- "$STAGE"
mkdir -p "$STAGE/sound" "$STAGE/images" "$STAGE/driver/bin/linux64"
cp -f "$BUILD_DIR/SpaceSync" "$STAGE/"
cp -f "$ROOT/3rdparty/OpenVR/bin/linux64/libopenvr_api.so" "$STAGE/"
cp -f "$ROOT/resources/LICENSE" "$ROOT/resources/LICENSE.txt" "$ROOT/resources/LICENSES" \
      "$ROOT/resources/manifest.vrmanifest" "$ROOT/NOTICE.md" "$STAGE/"
cp -f "$ROOT/resources/icon_v2.png" "$STAGE/icon.png"
cp -f "$ROOT"/src/sound/*.wav "$STAGE/sound/"
cp -f "$ROOT/resources/Basestation 2.0/"*.png "$STAGE/images/"
cp -f "$ROOT/dev-resources/driver/driver.vrdrivermanifest" "$STAGE/driver/"
cp -f "$BUILD_DIR/driver_spacesync.so" "$STAGE/driver/bin/linux64/"

if [[ "$ACTION" == build ]]; then
    echo
    echo "[build] OK: $STAGE"
    echo "[build] Run '$0 install' to install and register with SteamVR."
    exit 0
fi

steamvr_running && fail "Close SteamVR first (it keeps the old driver loaded)."
pkill -x SpaceSync 2>/dev/null && sleep 1 || true

echo "[build] Installing to $INSTALL_DIR ..."
mkdir -p "$INSTALL_DIR"
cp -a "$STAGE/." "$INSTALL_DIR/"

VRPATHREG="$(vrpathreg "$INSTALL_DIR")"
"$VRPATHREG" adddriver "$INSTALL_DIR/driver"
"$INSTALL_DIR/SpaceSync" -installmanifest || fail "Could not register the overlay with SteamVR."
"$INSTALL_DIR/SpaceSync" -activatemultipledrivers || fail "Could not enable activateMultipleDrivers."

echo
echo "[build] Installed. Start SteamVR; SpaceSync starts with it."
echo "[build] Desktop window without SteamVR: $INSTALL_DIR/SpaceSync -ui"
echo "[build] Logs: ${XDG_STATE_HOME:-$HOME/.local/state}/spacesync/"
