#!/bin/bash
#
# AudioDriver driver installer for macOS.
#
# Installs:
#   1. BlackHole 2ch virtual audio device (external dependency, GPLv3)
#   2. audio-driver-driver binary → /usr/local/bin/
#   3. Config directory → ~/Library/Application Support/AudioDriver/
#
# BlackHole is required on macOS for system audio loopback capture.
# It is installed as a separate package — not bundled with audio-driver.
#
# Usage:
#   sudo ./install.sh
#   sudo ./install.sh --uninstall
#   sudo ./install.sh --skip-blackhole    # Skip BlackHole install
#

set -e

BLACKHOLE_VERSION="0.6.1"
BLACKHOLE_PKG_URL="https://existential.audio/downloads/BlackHole2ch-${BLACKHOLE_VERSION}.pkg"
BLACKHOLE_PKG_ID="audio.existential.BlackHole2ch"
BLACKHOLE_SHA256="c829afa041a9f6e1b369c01953c8f079740dd1f02421109855829edc0d3c1988"

BINARY_NAME="audio-driver-driver"
BINARY_DEST="/usr/local/bin/${BINARY_NAME}"
CONFIG_DIR="${HOME}/Library/Application Support/AudioDriver"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

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

# ─── Check for BlackHole ─────────────────────────────────────

blackhole_installed() {
    # Check via pkgutil (package receipts)
    if pkgutil --pkg-info "${BLACKHOLE_PKG_ID}" &>/dev/null; then
        return 0
    fi
    # Check via HAL plugin directory
    if [ -d "/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver" ]; then
        return 0
    fi
    # Check via system_profiler
    if system_profiler SPAudioDataType 2>/dev/null | grep -qi "BlackHole"; then
        return 0
    fi
    return 1
}

install_blackhole() {
    if blackhole_installed; then
        log "BlackHole 2ch is already installed."
        return 0
    fi

    info "BlackHole 2ch is required for system audio loopback on macOS."
    info "BlackHole is a third-party open source driver (GPLv3) by Existential Audio."
    info "Homepage: https://existential.audio/blackhole/"
    echo ""

    # Try Homebrew first (preferred — handles updates)
    if command -v brew &>/dev/null; then
        log "Installing BlackHole 2ch via Homebrew..."
        # Run brew as the real user, not root
        REAL_USER="${SUDO_USER:-$USER}"
        if sudo -u "$REAL_USER" brew install --cask blackhole-2ch 2>/dev/null; then
            log "BlackHole 2ch installed via Homebrew."
            return 0
        fi
        warn "Homebrew install failed. Falling back to direct download..."
    fi

    # Direct download
    log "Downloading BlackHole 2ch v${BLACKHOLE_VERSION}..."
    local pkg_path="/tmp/BlackHole2ch-${BLACKHOLE_VERSION}.pkg"

    if ! curl -fSL -o "$pkg_path" "$BLACKHOLE_PKG_URL"; then
        error "Failed to download BlackHole from ${BLACKHOLE_PKG_URL}"
        error "Install BlackHole manually: https://existential.audio/blackhole/"
        return 1
    fi

    # Verify checksum
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

    # Install
    log "Installing BlackHole 2ch..."
    if installer -pkg "$pkg_path" -target / 2>/dev/null; then
        log "BlackHole 2ch installed successfully."
    else
        error "Failed to install BlackHole package."
        error "Install manually: https://existential.audio/blackhole/"
        rm -f "$pkg_path"
        return 1
    fi

    rm -f "$pkg_path"
    return 0
}

# ─── Uninstall ────────────────────────────────────────────────

if [ "$1" = "--uninstall" ]; then
    log "Uninstalling AudioDriver..."

    if [ -f "$BINARY_DEST" ]; then
        rm -f "$BINARY_DEST"
        log "Removed ${BINARY_DEST}"
    fi

    log ""
    warn "BlackHole 2ch was NOT uninstalled (other apps may depend on it)."
    warn "To uninstall BlackHole manually:"
    warn "  brew uninstall --cask blackhole-2ch"
    warn "  — or —"
    warn "  sudo rm -rf /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver"
    warn "  sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod"
    log ""
    warn "Config preserved at: ${CONFIG_DIR}"
    warn "To remove config: rm -rf '${CONFIG_DIR}'"

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

log "Installing AudioDriver driver..."
echo ""

# 1. Install BlackHole (external dependency)
if [ "$SKIP_BLACKHOLE" = false ]; then
    if ! install_blackhole; then
        warn "Continuing without BlackHole. System audio capture may not work."
        warn "Install BlackHole later: https://existential.audio/blackhole/"
    fi
    echo ""
fi

# 2. Install binary
BINARY_SOURCE="${SCRIPT_DIR}/../../build/audio-driver-driver"
if [ -f "$BINARY_SOURCE" ]; then
    log "Installing driver binary to ${BINARY_DEST}..."
    mkdir -p "$(dirname "$BINARY_DEST")"
    cp "$BINARY_SOURCE" "$BINARY_DEST"
    chmod 755 "$BINARY_DEST"
    log "Driver binary installed."
else
    warn "Driver binary not found at ${BINARY_SOURCE} — skipping."
    warn "Build it first: cmake --build build"
fi

# 3. Create config directory
REAL_HOME=$(eval echo "~${SUDO_USER:-$USER}")
REAL_CONFIG="${REAL_HOME}/Library/Application Support/AudioDriver"
if [ ! -d "$REAL_CONFIG" ]; then
    mkdir -p "$REAL_CONFIG"
    chown -R "${SUDO_USER:-$USER}" "$REAL_CONFIG"
    log "Config directory created: ${REAL_CONFIG}"

    cat > "${REAL_CONFIG}/config.ini" << 'EOF'
[mode]
mode=gateway

[auth]
backend_url=
email=
# password is never saved — enter at runtime

[gateway]
url=

[local]
socket_path=
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
    chown "${SUDO_USER:-$USER}" "${REAL_CONFIG}/config.ini"
    log "Default config written to ${REAL_CONFIG}/config.ini"
fi

# 4. Restart coreaudiod to detect BlackHole
log "Restarting coreaudiod..."
launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
    killall coreaudiod 2>/dev/null || true

# 5. Verify BlackHole is visible
sleep 2
if system_profiler SPAudioDataType 2>/dev/null | grep -qi "BlackHole"; then
    log "BlackHole virtual audio device detected!"
else
    warn "BlackHole not yet visible. A reboot may be required."
fi

echo ""
log "Installation complete!"
echo ""
log "Next steps:"
log "  Gateway mode (standalone):"
log "    audio-driver-driver --mode gateway --backend-url https://api.example.com --email you@example.com"
log ""
log "  Local mode (with Sulla Desktop):"
log "    audio-driver-driver --mode local"
log ""
log "  List devices:"
log "    audio-driver-driver --list-devices"
