#!/bin/bash
#
# Audio Driver installer for macOS (idempotent).
#
# Safe to run repeatedly — checks current state and only acts on
# what's broken or missing. Running it again will fix itself.
#
# Installs:
#   1. BlackHole 2ch virtual audio device (external dependency, GPLv3)
#   2. audio-driver binary → /usr/local/bin/
#   3. Config directory → ~/Library/Application Support/AudioDriver/
#   4. launchd service → auto-starts on boot
#
# Usage:
#   sudo ./install.sh
#   sudo ./install.sh --uninstall
#   sudo ./install.sh --skip-blackhole
#

set -euo pipefail

BLACKHOLE_VERSION="0.6.1"
BLACKHOLE_PKG_URL="https://existential.audio/downloads/BlackHole2ch-${BLACKHOLE_VERSION}.pkg"
BLACKHOLE_PKG_ID="audio.existential.BlackHole2ch"
BLACKHOLE_SHA256="c829afa041a9f6e1b369c01953c8f079740dd1f02421109855829edc0d3c1988"

BINARY_NAME="audio-driver"
BINARY_DEST="/usr/local/bin/${BINARY_NAME}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Resolve the real (non-root) user — critical for brew and config ownership
REAL_USER="${SUDO_USER:-$USER}"
REAL_HOME=$(eval echo "~${REAL_USER}")
REAL_CONFIG="${REAL_HOME}/Library/Application Support/AudioDriver"

PLIST_LABEL="com.audiodriver.agent"
OLD_PLIST_PATH="/Library/LaunchDaemons/${PLIST_LABEL}.plist"
# LaunchAgent runs as the user — required for CoreAudio session access
PLIST_DIR="${REAL_HOME}/Library/LaunchAgents"
PLIST_PATH="${PLIST_DIR}/${PLIST_LABEL}.plist"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()   { echo -e "${GREEN}[audio-driver]${NC} $1"; }
warn()  { echo -e "${YELLOW}[audio-driver]${NC} $1"; }
error() { echo -e "${RED}[audio-driver]${NC} $1" >&2; }
info()  { echo -e "${CYAN}[audio-driver]${NC} $1"; }

# Track outcomes for the summary
BLACKHOLE_OK=false
BINARY_OK=false
SERVICE_OK=false

# Track whether something was freshly installed (vs already present)
BLACKHOLE_JUST_INSTALLED=false
BINARY_UPDATED=false

# ─── Resolve Homebrew under sudo ──────────────────────────────
# brew is installed per-user and not on root's PATH. We must
# find it explicitly so that `sudo -u $REAL_USER brew ...` works.

resolve_brew() {
    local candidates=(
        "/opt/homebrew/bin/brew"
        "/usr/local/bin/brew"
    )
    local user_brew
    user_brew=$(sudo -u "$REAL_USER" bash -lc 'command -v brew' 2>/dev/null || true)
    if [ -n "$user_brew" ]; then
        candidates+=("$user_brew")
    fi
    for candidate in "${candidates[@]}"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# ─── Check for BlackHole ─────────────────────────────────────

blackhole_installed() {
    if pkgutil --pkg-info "${BLACKHOLE_PKG_ID}" &>/dev/null; then
        log "  (detected via pkgutil receipt)"
        return 0
    fi
    if [ -d "/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver" ]; then
        log "  (detected via HAL plugin directory)"
        return 0
    fi
    if system_profiler SPAudioDataType 2>/dev/null | grep -qi "BlackHole"; then
        log "  (detected via system_profiler)"
        return 0
    fi

    # Check if brew thinks it's installed but OS doesn't see it (stale state).
    # This happens when driver files are manually deleted without `brew uninstall`.
    # Clean up the stale receipt so the next install attempt doesn't conflict.
    local brew_bin
    if brew_bin=$(resolve_brew); then
        if sudo -u "$REAL_USER" "$brew_bin" list --cask blackhole-2ch &>/dev/null; then
            warn "Homebrew reports blackhole-2ch as installed, but the driver is missing."
            warn "Cleaning up stale Homebrew state..."
            sudo -u "$REAL_USER" "$brew_bin" uninstall --cask blackhole-2ch --force 2>/dev/null || true
        fi
    fi

    return 1
}

install_blackhole() {
    log "Checking for BlackHole 2ch..."
    if blackhole_installed; then
        log "BlackHole 2ch is already installed."
        BLACKHOLE_OK=true
        return 0
    fi

    info "BlackHole 2ch is required for system audio loopback on macOS."
    info "BlackHole is a third-party open source driver (GPLv3) by Existential Audio."
    info "Homepage: https://existential.audio/blackhole/"
    echo ""

    # Try Homebrew first (preferred — clean uninstall path)
    local brew_bin
    if brew_bin=$(resolve_brew); then
        log "Installing BlackHole 2ch via Homebrew..."
        local brew_output
        if brew_output=$(sudo -u "$REAL_USER" "$brew_bin" install --cask blackhole-2ch 2>&1); then
            log "BlackHole 2ch installed via Homebrew."
            BLACKHOLE_OK=true
            BLACKHOLE_JUST_INSTALLED=true
            return 0
        else
            warn "Homebrew install failed:"
            echo "$brew_output" | tail -5 | while IFS= read -r line; do warn "  $line"; done
            warn "Falling back to direct download..."
        fi
    else
        info "Homebrew not found. Using direct download..."
    fi

    # Direct download fallback
    log "Downloading BlackHole 2ch v${BLACKHOLE_VERSION}..."
    local pkg_path="/tmp/BlackHole2ch-${BLACKHOLE_VERSION}.pkg"

    if ! curl -fSL -o "$pkg_path" "$BLACKHOLE_PKG_URL"; then
        error "Failed to download BlackHole from ${BLACKHOLE_PKG_URL}"
        error "Install BlackHole manually: https://existential.audio/blackhole/"
        return 1
    fi

    local actual_sha
    actual_sha=$(shasum -a 256 "$pkg_path" | awk '{print $1}')
    if [ "$actual_sha" != "$BLACKHOLE_SHA256" ]; then
        error "Checksum mismatch for BlackHole package!"
        error "Expected: ${BLACKHOLE_SHA256}"
        error "Got:      ${actual_sha}"
        rm -f "$pkg_path"
        return 1
    fi
    log "Checksum verified."

    log "Installing BlackHole 2ch..."
    local installer_output
    if installer_output=$(installer -pkg "$pkg_path" -target / 2>&1); then
        log "BlackHole 2ch installed successfully."
        BLACKHOLE_OK=true
        BLACKHOLE_JUST_INSTALLED=true
    else
        error "Failed to install BlackHole package:"
        echo "$installer_output" | while IFS= read -r line; do error "  $line"; done
        error "Install manually: https://existential.audio/blackhole/"
        rm -f "$pkg_path"
        return 1
    fi

    rm -f "$pkg_path"
    return 0
}

# ─── Uninstall ────────────────────────────────────────────────

if [ "${1:-}" = "--uninstall" ]; then
    log "Uninstalling Audio Driver..."

    # Stop and remove launchd service (check both old system daemon and new user agent)
    local REAL_UID
    REAL_UID=$(id -u "$REAL_USER")
    launchctl bootout system/"${PLIST_LABEL}" 2>/dev/null || true
    sudo -u "$REAL_USER" launchctl bootout "gui/${REAL_UID}/${PLIST_LABEL}" 2>/dev/null || true
    rm -f "$OLD_PLIST_PATH" "$PLIST_PATH"
    log "Service removed."

    if [ -f "$BINARY_DEST" ]; then
        rm -f "$BINARY_DEST"
        log "Removed ${BINARY_DEST}"
    fi

    # Remove Multi-Output Device
    local swift_src="${SCRIPT_DIR}/create-multi-output.swift"
    local swift_bin="/tmp/sulla-create-multi-output"
    if [ -f "$swift_src" ]; then
        if swiftc -O "$swift_src" -o "$swift_bin" 2>/dev/null; then
            if "$swift_bin" --remove 2>&1; then
                log "Multi-Output Device removed."
            fi
            rm -f "$swift_bin"
        fi
    fi

    # Clean up socket
    rm -f /tmp/audio-driver.sock

    echo ""
    warn "BlackHole 2ch was NOT uninstalled (other apps may depend on it)."
    warn "To uninstall BlackHole manually:"
    warn "  brew uninstall --cask blackhole-2ch"
    warn "  — or —"
    warn "  sudo rm -rf /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver"
    warn "  sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod"
    echo ""
    warn "Config preserved at: ${REAL_CONFIG}"
    warn "To remove config: rm -rf '${REAL_CONFIG}'"

    log "Uninstall complete."
    exit 0
fi

# ─── Install ──────────────────────────────────────────────────

if [ "$(id -u)" -ne 0 ]; then
    error "This installer must be run as root (sudo)."
    exit 1
fi

SKIP_BLACKHOLE=false
for arg in "$@"; do
    if [ "$arg" = "--skip-blackhole" ]; then
        SKIP_BLACKHOLE=true
    fi
done

log "Installing Audio Driver..."
echo ""

# ─── Step 1: BlackHole ────────────────────────────────────────

if [ "$SKIP_BLACKHOLE" = false ]; then
    if ! install_blackhole; then
        warn "Continuing without BlackHole. System audio capture may not work."
        warn "Install BlackHole later: https://existential.audio/blackhole/"
    fi
    echo ""
fi

# ─── Step 2: Build and install binary ─────────────────────────

PROJECT_ROOT="${SCRIPT_DIR}/../.."
BUILD_DIR="${PROJECT_ROOT}/build"
BINARY_SOURCE="${BUILD_DIR}/audio-driver"

resolve_cmake() {
    # Check common locations (root's PATH won't include Homebrew)
    local candidates=(
        "/opt/homebrew/bin/cmake"
        "/usr/local/bin/cmake"
    )
    # Ask the real user's shell where cmake is
    local user_cmake
    user_cmake=$(sudo -u "$REAL_USER" bash -lc 'command -v cmake' 2>/dev/null || true)
    if [ -n "$user_cmake" ]; then
        candidates+=("$user_cmake")
    fi

    for candidate in "${candidates[@]}"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done

    # Not found — try to install via Homebrew
    warn "cmake not found. Attempting to install via Homebrew..."
    local local_brew=""
    if local_brew=$(resolve_brew); then
        if sudo -u "$REAL_USER" "$local_brew" install cmake 2>&1; then
            local installed_cmake="$(dirname "$local_brew")/cmake"
            if [ -x "$installed_cmake" ]; then
                echo "$installed_cmake"
                return 0
            fi
            installed_cmake="$("$local_brew" --prefix)/bin/cmake"
            if [ -x "$installed_cmake" ]; then
                echo "$installed_cmake"
                return 0
            fi
        fi
    fi

    return 1
}

build_binary() {
    log "Building audio-driver from source..."

    local cmake_bin
    if ! cmake_bin=$(resolve_cmake); then
        error "cmake is required to build audio-driver."
        error "Install it: brew install cmake"
        error "Then re-run this installer."
        return 1
    fi

    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    log "Running cmake configure..."
    if ! "$cmake_bin" -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release </dev/null 2>&1; then
        error "cmake configure failed. Full output above."
        return 1
    fi

    log "Running cmake build..."
    if ! "$cmake_bin" --build "$BUILD_DIR" --target audio-driver 2>&1; then
        error "cmake build failed. Full output above."
        return 1
    fi

    if [ ! -f "$BINARY_SOURCE" ]; then
        error "Build completed but binary not produced."
        return 1
    fi

    log "Build successful."
    return 0
}

install_binary() {
    # Step A: Ensure we have a build artifact
    if [ ! -f "$BINARY_SOURCE" ]; then
        if ! build_binary; then
            if [ -f "$BINARY_DEST" ] && [ -x "$BINARY_DEST" ]; then
                warn "Build failed but existing binary at ${BINARY_DEST} is usable."
                BINARY_OK=true
                return 0
            fi
            error "No binary available. The service cannot start without it."
            return 1
        fi
    fi

    # Step B: Always copy to destination — this is the step that must not be skipped
    if [ -f "$BINARY_DEST" ] && cmp -s "$BINARY_SOURCE" "$BINARY_DEST"; then
        log "Binary at ${BINARY_DEST} is up to date."
    else
        log "Installing binary to ${BINARY_DEST}..."
        mkdir -p "$(dirname "$BINARY_DEST")"
        cp "$BINARY_SOURCE" "$BINARY_DEST"
        chmod 755 "$BINARY_DEST"
        log "Binary installed."
        BINARY_UPDATED=true
    fi

    # Step C: Verify it's actually there and executable
    if [ ! -f "$BINARY_DEST" ]; then
        error "Binary copy failed — ${BINARY_DEST} does not exist after cp."
        error "Check permissions on $(dirname "$BINARY_DEST")"
        return 1
    fi

    if [ ! -x "$BINARY_DEST" ]; then
        error "Binary at ${BINARY_DEST} is not executable after chmod."
        return 1
    fi

    BINARY_OK=true
    return 0
}

install_binary
echo ""

# ─── Step 3: Ensure config exists ─────────────────────────────

mkdir -p "$REAL_CONFIG"
chown "${REAL_USER}" "$REAL_CONFIG"

if [ ! -f "${REAL_CONFIG}/config.ini" ]; then
    log "Creating default config..."
    cat > "${REAL_CONFIG}/config.ini" << 'EOF'
[mode]
mode=local

[auth]
backend_url=
email=

[gateway]
url=

[local]
socket_path=/tmp/audio-driver.sock
port=0

[audio]
preferred_device=
chunk_interval_ms=200
target_sample_rate=16000
target_bit_depth=16
target_channels=1
capture_mic=true
capture_speaker=true
auto_start=true

[logging]
log_level=info
log_audio_diagnostics=true
EOF
    chown "${REAL_USER}" "${REAL_CONFIG}/config.ini"
    log "Default config created: local mode (no credentials needed)"
else
    log "Config exists at ${REAL_CONFIG}/config.ini"
fi

# ─── Step 4: coreaudiod (only if BlackHole was just installed) ─

if [ "$BLACKHOLE_JUST_INSTALLED" = true ]; then
    log "Restarting coreaudiod to detect newly installed BlackHole..."
    launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
        killall coreaudiod 2>/dev/null || true

    tries=0
    while [ $tries -lt 5 ]; do
        sleep 1
        if system_profiler SPAudioDataType 2>/dev/null | grep -qi "BlackHole"; then
            log "BlackHole virtual audio device detected!"
            break
        fi
        tries=$((tries + 1))
    done
    if [ $tries -eq 5 ]; then
        warn "BlackHole not yet visible. A reboot may be required."
    fi
fi

# ─── Step 5: Multi-Output Device (routes system audio → BlackHole) ─

MULTI_OUTPUT_OK=false

setup_multi_output() {
    if [ "$BLACKHOLE_OK" != true ] && [ "$SKIP_BLACKHOLE" = true ]; then
        warn "BlackHole not installed — skipping Multi-Output Device."
        return 1
    fi

    local swift_src="${SCRIPT_DIR}/create-multi-output.swift"
    local swift_bin="/tmp/sulla-create-multi-output"

    if [ ! -f "$swift_src" ]; then
        error "create-multi-output.swift not found at ${swift_src}"
        return 1
    fi

    # Compile the Swift helper
    log "Compiling Multi-Output Device helper..."
    if ! swiftc -O "$swift_src" -o "$swift_bin" 2>&1; then
        error "Failed to compile create-multi-output.swift"
        return 1
    fi

    # Check if it already exists
    if "$swift_bin" --check 2>/dev/null; then
        log "Multi-Output Device already exists."
        MULTI_OUTPUT_OK=true
        return 0
    fi

    # Create it (must run as the real user for audio session access)
    log "Creating Multi-Output Device (mirrors speakers → BlackHole)..."
    local output
    if output=$(sudo -u "$REAL_USER" "$swift_bin" 2>&1); then
        echo "$output" | while IFS= read -r line; do log "  $line"; done
        MULTI_OUTPUT_OK=true
    else
        echo "$output" | while IFS= read -r line; do warn "  $line"; done
        warn "Multi-Output Device creation failed."
        warn "You can create it manually in Audio MIDI Setup:"
        warn "  1. Open /Applications/Utilities/Audio MIDI Setup.app"
        warn "  2. Click '+' → Create Multi-Output Device"
        warn "  3. Check your speakers AND BlackHole 2ch"
        warn "  4. Set the Multi-Output Device as your system output"
        return 1
    fi

    rm -f "$swift_bin"
    return 0
}

setup_multi_output
echo ""

# ─── Step 6: Ensure launchd service is running ────────────────

ensure_service() {
    if [ "$BINARY_OK" != true ]; then
        warn "Binary not installed — cannot start service."
        return 1
    fi

    local REAL_UID
    REAL_UID=$(id -u "$REAL_USER")

    # Clean up stale socket (exists but no process behind it)
    if [ -S /tmp/audio-driver.sock ]; then
        if ! lsof /tmp/audio-driver.sock &>/dev/null; then
            warn "Stale socket found. Removing /tmp/audio-driver.sock"
            rm -f /tmp/audio-driver.sock
        fi
    fi

    # Migrate: remove old system-level LaunchDaemon if present
    if [ -f "$OLD_PLIST_PATH" ]; then
        log "Migrating from system daemon to user agent..."
        launchctl bootout system/"${PLIST_LABEL}" 2>/dev/null || true
        rm -f "$OLD_PLIST_PATH"
    fi

    # Check if the service is already running and healthy
    if [ "$BINARY_UPDATED" != true ] && sudo -u "$REAL_USER" launchctl print "gui/${REAL_UID}/${PLIST_LABEL}" &>/dev/null && [ -S /tmp/audio-driver.sock ]; then
        log "Service is already running and socket is live."
        SERVICE_OK=true
        return 0
    fi

    if [ "$BINARY_UPDATED" = true ]; then
        log "Binary was updated — restarting service to pick up new version..."
    fi

    # Ensure LaunchAgents directory exists
    mkdir -p "$PLIST_DIR"
    chown "${REAL_USER}" "$PLIST_DIR"

    # Write or update the plist (LaunchAgent — runs as user for CoreAudio access)
    cat > "$PLIST_PATH" << PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${PLIST_LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${BINARY_DEST}</string>
        <string>--mode</string>
        <string>local</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/audio-driver.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/audio-driver.log</string>
</dict>
</plist>
PLISTEOF

    chmod 644 "$PLIST_PATH"
    chown "${REAL_USER}" "$PLIST_PATH"

    # Stop if registered
    sudo -u "$REAL_USER" launchctl bootout "gui/${REAL_UID}/${PLIST_LABEL}" 2>/dev/null || true

    # Start as user agent
    if sudo -u "$REAL_USER" launchctl bootstrap "gui/${REAL_UID}" "$PLIST_PATH" 2>/dev/null; then
        log "Service started (as user: ${REAL_USER})."
    else
        if sudo -u "$REAL_USER" launchctl kickstart -k "gui/${REAL_UID}/${PLIST_LABEL}" 2>/dev/null; then
            log "Service restarted (as user: ${REAL_USER})."
        else
            error "Failed to start user agent."
            error "Try manually: launchctl bootstrap gui/\$(id -u) ${PLIST_PATH}"
            return 1
        fi
    fi

    # Verify the socket appears
    tries=0
    while [ $tries -lt 5 ]; do
        sleep 1
        if [ -S /tmp/audio-driver.sock ]; then
            log "Socket /tmp/audio-driver.sock is live!"
            SERVICE_OK=true
            return 0
        fi
        tries=$((tries + 1))
    done

    if sudo -u "$REAL_USER" launchctl print "gui/${REAL_UID}/${PLIST_LABEL}" &>/dev/null; then
        warn "Service is running but socket not yet available."
        warn "Check logs: cat /tmp/audio-driver.log"
        SERVICE_OK=true
        return 0
    fi

    error "Service failed to start."
    error "Check logs: cat /tmp/audio-driver.log"
    return 1
}

ensure_service

# ─── Summary ──────────────────────────────────────────────────

echo ""
echo "─────────────────────────────────────────────────────"

FAILURES=0

if [ "$BLACKHOLE_OK" = true ]; then
    log "  BlackHole 2ch .......... OK"
elif [ "$SKIP_BLACKHOLE" = true ]; then
    warn "  BlackHole 2ch .......... SKIPPED"
else
    error "  BlackHole 2ch .......... FAILED"
    FAILURES=$((FAILURES + 1))
fi

if [ "$MULTI_OUTPUT_OK" = true ]; then
    log "  Multi-Output Device .... OK  (Sulla Audio Mirror)"
else
    warn "  Multi-Output Device .... MANUAL SETUP NEEDED"
fi

if [ "$BINARY_OK" = true ]; then
    log "  audio-driver binary .... OK  (${BINARY_DEST})"
else
    error "  audio-driver binary .... FAILED"
    FAILURES=$((FAILURES + 1))
fi

if [ "$SERVICE_OK" = true ]; then
    log "  launchd service ........ OK  (${PLIST_LABEL})"
else
    error "  launchd service ........ FAILED"
    FAILURES=$((FAILURES + 1))
fi

echo "─────────────────────────────────────────────────────"

if [ "$FAILURES" -gt 0 ]; then
    echo ""
    error "Installation completed with ${FAILURES} failure(s). Review errors above."
    exit 1
fi

echo ""
log "Installation complete! Audio driver is running."
echo ""
log "Desktop apps connect automatically via /tmp/audio-driver.sock"
echo ""
log "Commands:"
log "  audio-driver --list-devices       List audio devices"
log "  audio-driver --configure          Set up gateway mode (credentials)"
echo ""
log "Service management:"
log "  launchctl kickstart -k gui/\$(id -u)/${PLIST_LABEL}   Restart"
log "  launchctl bootout gui/\$(id -u)/${PLIST_LABEL}        Stop"
log "  cat /tmp/audio-driver.log                              View logs"
