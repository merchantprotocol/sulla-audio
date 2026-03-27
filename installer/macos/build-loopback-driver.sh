#!/bin/bash
#
# build-loopback-driver.sh — builds a custom-named BlackHole fork
# ("SullaLoopback") that is hidden from System Settings.
#
# This compiles BlackHole from source with a custom driver name, bundle ID,
# and a unique CFPlugIn UUID so it can coexist with stock BlackHole installs.
# The device is marked hidden so it never appears in Sound preferences.
#
# Usage:
#   ./build-loopback-driver.sh              # build only
#   sudo ./build-loopback-driver.sh install # build + install to /Library/Audio/Plug-Ins/HAL/
#   sudo ./build-loopback-driver.sh remove  # uninstall
#
# Requires: Xcode command-line tools (xcodebuild)
#

set -euo pipefail

DRIVER_NAME="SullaLoopback"
CHANNELS=2
VARIANT="${DRIVER_NAME}${CHANNELS}ch"
BUNDLE_ID="com.sulla.${VARIANT}"
DRIVER_BUNDLE="${VARIANT}.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
INSTALL_PATH="${HAL_DIR}/${DRIVER_BUNDLE}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="/tmp/sulla-loopback-build"
BLACKHOLE_REPO="https://github.com/ExistentialAudio/BlackHole.git"
BLACKHOLE_TAG="v0.6.1"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[loopback]${NC} $1"; }
warn()  { echo -e "${YELLOW}[loopback]${NC} $1"; }
error() { echo -e "${RED}[loopback]${NC} $1" >&2; }

# ─── Remove ──────────────────────────────────────────────────

if [ "${1:-}" = "remove" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        error "Removal requires root (sudo)."
        exit 1
    fi
    if [ -d "$INSTALL_PATH" ]; then
        log "Removing ${INSTALL_PATH}..."
        rm -rf "$INSTALL_PATH"
        log "Restarting coreaudiod..."
        launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
            killall coreaudiod 2>/dev/null || true
        log "SullaLoopback driver removed."
    else
        log "SullaLoopback driver not installed. Nothing to remove."
    fi
    exit 0
fi

# ─── Build ───────────────────────────────────────────────────

check_xcode() {
    if ! command -v xcodebuild &>/dev/null; then
        error "xcodebuild not found. Install Xcode command-line tools:"
        error "  xcode-select --install"
        exit 1
    fi
}

clone_blackhole() {
    if [ -d "${WORK_DIR}/BlackHole" ]; then
        log "BlackHole source already cloned."
        return
    fi
    log "Cloning BlackHole ${BLACKHOLE_TAG}..."
    mkdir -p "$WORK_DIR"
    git clone --depth 1 --branch "$BLACKHOLE_TAG" "$BLACKHOLE_REPO" "${WORK_DIR}/BlackHole" 2>&1
}

build_driver() {
    local src="${WORK_DIR}/BlackHole"
    local build_dir="${WORK_DIR}/build"

    log "Building ${VARIANT} (hidden from System Settings)..."

    # Generate a unique CFPlugIn UUID so this can coexist with stock BlackHole
    local uuid
    uuid=$(uuidgen)

    xcodebuild \
        -project "${src}/BlackHole.xcodeproj" \
        -configuration Release \
        -target BlackHole \
        CONFIGURATION_BUILD_DIR="$build_dir" \
        PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
        CODE_SIGN_IDENTITY="" \
        CODE_SIGNING_REQUIRED=NO \
        CODE_SIGNING_ALLOWED=NO \
        GCC_PREPROCESSOR_DEFINITIONS='$(GCC_PREPROCESSOR_DEFINITIONS)
            kNumber_Of_Channels='"$CHANNELS"'
            kPlugIn_BundleID=\"'"$BUNDLE_ID"'\"
            kDriver_Name=\"'"$DRIVER_NAME"'\"
            kDevice_IsHidden=true
            kDevice2_IsHidden=true' \
        2>&1

    if [ ! -d "${build_dir}/BlackHole.driver" ]; then
        error "Build failed — no driver bundle produced."
        exit 1
    fi

    # Rename the bundle to our custom name
    rm -rf "${build_dir}/${DRIVER_BUNDLE}"
    mv "${build_dir}/BlackHole.driver" "${build_dir}/${DRIVER_BUNDLE}"

    # Replace the default CFPlugIn UUID with our unique one so multiple
    # BlackHole variants can coexist on the same system
    local plist="${build_dir}/${DRIVER_BUNDLE}/Contents/Info.plist"
    if [ -f "$plist" ]; then
        # Replace the stock BlackHole UUID
        sed -i '' "s/e395c745-4eea-4d94-bb92-46224221047c/${uuid}/g" "$plist"
        log "Set unique CFPlugIn UUID: ${uuid}"
    fi

    log "Build complete: ${build_dir}/${DRIVER_BUNDLE}"
}

install_driver() {
    local build_dir="${WORK_DIR}/build"

    if [ "$(id -u)" -ne 0 ]; then
        error "Installation requires root (sudo)."
        exit 1
    fi

    if [ ! -d "${build_dir}/${DRIVER_BUNDLE}" ]; then
        error "Driver not built. Run build first."
        exit 1
    fi

    # Remove old version if present
    if [ -d "$INSTALL_PATH" ]; then
        log "Removing previous ${DRIVER_BUNDLE}..."
        rm -rf "$INSTALL_PATH"
    fi

    log "Installing to ${INSTALL_PATH}..."
    cp -R "${build_dir}/${DRIVER_BUNDLE}" "$INSTALL_PATH"
    chmod -R 755 "$INSTALL_PATH"

    log "Restarting coreaudiod..."
    launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
        killall coreaudiod 2>/dev/null || true

    # Wait for the device to appear
    local tries=0
    while [ $tries -lt 5 ]; do
        sleep 1
        if system_profiler SPAudioDataType 2>/dev/null | grep -qi "$DRIVER_NAME"; then
            log "SullaLoopback virtual audio device detected!"
            break
        fi
        tries=$((tries + 1))
    done
    if [ $tries -eq 5 ]; then
        warn "Device not yet visible. A reboot may be required."
    fi

    log "SullaLoopback installed successfully."
}

# ─── Main ────────────────────────────────────────────────────

check_xcode
clone_blackhole
build_driver

if [ "${1:-}" = "install" ]; then
    install_driver
else
    log ""
    log "Driver built at: ${WORK_DIR}/build/${DRIVER_BUNDLE}"
    log "To install: sudo $0 install"
fi
