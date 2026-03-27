#!/bin/bash
#
# Audio Driver installer for macOS (idempotent).
#
# Safe to run repeatedly — checks current state and only acts on
# what's broken or missing. Running it again will fix itself.
#
# Downloads pre-built binaries from GitHub Releases. Falls back to
# building from source if the download fails (requires Xcode + cmake).
#
# Installs:
#   1. SullaLoopback virtual audio device (hidden from System Settings)
#   2. audio-driver binary → /usr/local/bin/
#   3. Config directory → ~/Library/Application Support/AudioDriver/
#   4. launchd service → auto-starts on boot
#
# Usage:
#   sudo ./install.sh
#   sudo ./install.sh --uninstall
#   sudo ./install.sh --skip-loopback
#   sudo ./install.sh --from-source      # force build from source
#

set -euo pipefail

GITHUB_REPO="merchantprotocol/sulla-audio"
LOOPBACK_DRIVER_NAME="SullaLoopback2ch"
LOOPBACK_HAL_PATH="/Library/Audio/Plug-Ins/HAL/${LOOPBACK_DRIVER_NAME}.driver"

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
LOOPBACK_OK=false
BINARY_OK=false
SERVICE_OK=false

# Track whether something was freshly installed (vs already present)
LOOPBACK_JUST_INSTALLED=false
BINARY_UPDATED=false

# ─── GitHub Release helpers ──────────────────────────────────

# Fetch the download URL for an asset from the latest GitHub Release.
# Usage: get_release_asset_url <asset-name-pattern>
get_release_asset_url() {
    local pattern="$1"
    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    local release_json

    release_json=$(curl -fsSL "$api_url" 2>/dev/null) || return 1

    # Extract the browser_download_url matching the pattern
    echo "$release_json" \
        | grep -o '"browser_download_url": *"[^"]*'"$pattern"'[^"]*"' \
        | head -1 \
        | sed 's/"browser_download_url": *"//' \
        | sed 's/"$//'
}

# Download a file, verify with companion .sha256 if available.
# Usage: download_and_verify <url> <dest-path>
download_and_verify() {
    local url="$1"
    local dest="$2"

    if ! curl -fSL -o "$dest" "$url"; then
        return 1
    fi

    # Try to verify checksum
    local sha_url="${url}.sha256"
    local sha_file="${dest}.sha256"
    if curl -fsSL -o "$sha_file" "$sha_url" 2>/dev/null; then
        local expected actual
        expected=$(awk '{print $1}' "$sha_file")
        actual=$(shasum -a 256 "$dest" | awk '{print $1}')
        rm -f "$sha_file"
        if [ "$expected" != "$actual" ]; then
            error "Checksum mismatch!"
            error "Expected: ${expected}"
            error "Got:      ${actual}"
            rm -f "$dest"
            return 1
        fi
        log "Checksum verified."
    fi

    return 0
}

# ─── Resolve Homebrew under sudo ──────────────────────────────

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

# ─── SullaLoopback driver ───────────────────────────────────

loopback_installed() {
    if [ -d "$LOOPBACK_HAL_PATH" ]; then
        log "  (detected via HAL plugin directory)"
        return 0
    fi
    if system_profiler SPAudioDataType 2>/dev/null | grep -qi "SullaLoopback"; then
        log "  (detected via system_profiler)"
        return 0
    fi
    return 1
}

install_loopback_from_release() {
    log "Downloading pre-built SullaLoopback driver..."

    local url
    url=$(get_release_asset_url "${LOOPBACK_DRIVER_NAME}.driver.tar.gz") || return 1
    if [ -z "$url" ]; then return 1; fi

    local tmp_tar="/tmp/${LOOPBACK_DRIVER_NAME}.driver.tar.gz"
    if ! download_and_verify "$url" "$tmp_tar"; then
        rm -f "$tmp_tar"
        return 1
    fi

    # Extract to HAL directory
    if [ -d "$LOOPBACK_HAL_PATH" ]; then
        rm -rf "$LOOPBACK_HAL_PATH"
    fi

    tar xzf "$tmp_tar" -C /Library/Audio/Plug-Ins/HAL/
    rm -f "$tmp_tar"

    if [ ! -d "$LOOPBACK_HAL_PATH" ]; then
        error "Extraction succeeded but driver bundle not found."
        return 1
    fi

    chmod -R 755 "$LOOPBACK_HAL_PATH"
    log "SullaLoopback driver installed from release."
    return 0
}

install_loopback_from_source() {
    local build_script="${SCRIPT_DIR}/build-loopback-driver.sh"
    if [ ! -f "$build_script" ]; then
        error "build-loopback-driver.sh not found — cannot build from source."
        return 1
    fi

    log "Building SullaLoopback driver from source..."
    if bash "$build_script" install 2>&1; then
        log "SullaLoopback driver built and installed."
        return 0
    else
        error "Source build failed. Ensure Xcode CLI tools: xcode-select --install"
        return 1
    fi
}

install_loopback() {
    log "Checking for SullaLoopback driver..."
    if loopback_installed; then
        log "SullaLoopback driver is already installed."
        LOOPBACK_OK=true
        return 0
    fi

    info "SullaLoopback is required for system audio loopback on macOS."
    echo ""

    if [ "$FROM_SOURCE" = false ]; then
        if install_loopback_from_release; then
            LOOPBACK_OK=true
            LOOPBACK_JUST_INSTALLED=true
            return 0
        fi
        warn "Pre-built download unavailable. Falling back to source build..."
    fi

    if install_loopback_from_source; then
        LOOPBACK_OK=true
        LOOPBACK_JUST_INSTALLED=true
        return 0
    fi

    return 1
}

# ─── audio-driver binary ────────────────────────────────────

install_binary_from_release() {
    log "Downloading pre-built audio-driver binary..."

    local url
    url=$(get_release_asset_url "audio-driver-macos-arm64.tar.gz") || return 1
    if [ -z "$url" ]; then return 1; fi

    local tmp_tar="/tmp/audio-driver-macos-arm64.tar.gz"
    if ! download_and_verify "$url" "$tmp_tar"; then
        rm -f "$tmp_tar"
        return 1
    fi

    tar xzf "$tmp_tar" -C /tmp/
    rm -f "$tmp_tar"

    if [ ! -f "/tmp/audio-driver" ]; then
        error "Extraction succeeded but binary not found."
        return 1
    fi

    mkdir -p "$(dirname "$BINARY_DEST")"
    mv /tmp/audio-driver "$BINARY_DEST"
    chmod 755 "$BINARY_DEST"
    log "audio-driver installed from release."
    return 0
}

resolve_cmake() {
    local candidates=(
        "/opt/homebrew/bin/cmake"
        "/usr/local/bin/cmake"
    )
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

install_binary_from_source() {
    local project_root="${SCRIPT_DIR}/../.."
    local build_dir="${project_root}/build"

    log "Building audio-driver from source..."

    local cmake_bin
    if ! cmake_bin=$(resolve_cmake); then
        error "cmake is required to build audio-driver."
        error "Install it: brew install cmake"
        return 1
    fi

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    log "Running cmake configure..."
    if ! "$cmake_bin" -S "$project_root" -B "$build_dir" -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release </dev/null 2>&1; then
        error "cmake configure failed."
        return 1
    fi

    log "Running cmake build..."
    if ! "$cmake_bin" --build "$build_dir" --target audio-driver 2>&1; then
        error "cmake build failed."
        return 1
    fi

    if [ ! -f "${build_dir}/audio-driver" ]; then
        error "Build completed but binary not produced."
        return 1
    fi

    mkdir -p "$(dirname "$BINARY_DEST")"
    cp "${build_dir}/audio-driver" "$BINARY_DEST"
    chmod 755 "$BINARY_DEST"
    log "audio-driver built and installed."
    return 0
}

install_binary() {
    # Check if already up to date (when installing from release, always update)
    if [ "$FROM_SOURCE" = false ]; then
        if install_binary_from_release; then
            BINARY_OK=true
            BINARY_UPDATED=true
            return 0
        fi
        warn "Pre-built download unavailable. Falling back to source build..."
    fi

    # Source build path
    local project_root="${SCRIPT_DIR}/../.."
    local build_dir="${project_root}/build"
    local binary_source="${build_dir}/audio-driver"

    if [ ! -f "$binary_source" ]; then
        if ! install_binary_from_source; then
            if [ -f "$BINARY_DEST" ] && [ -x "$BINARY_DEST" ]; then
                warn "Build failed but existing binary at ${BINARY_DEST} is usable."
                BINARY_OK=true
                return 0
            fi
            error "No binary available. The service cannot start without it."
            return 1
        fi
        BINARY_OK=true
        BINARY_UPDATED=true
        return 0
    fi

    # Binary already built locally — install it
    if [ -f "$BINARY_DEST" ] && cmp -s "$binary_source" "$BINARY_DEST"; then
        log "Binary at ${BINARY_DEST} is up to date."
    else
        log "Installing binary to ${BINARY_DEST}..."
        mkdir -p "$(dirname "$BINARY_DEST")"
        cp "$binary_source" "$BINARY_DEST"
        chmod 755 "$BINARY_DEST"
        log "Binary installed."
        BINARY_UPDATED=true
    fi

    if [ ! -f "$BINARY_DEST" ] || [ ! -x "$BINARY_DEST" ]; then
        error "Binary installation failed."
        return 1
    fi

    BINARY_OK=true
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

    # Remove SullaLoopback driver
    if [ -d "$LOOPBACK_HAL_PATH" ]; then
        rm -rf "$LOOPBACK_HAL_PATH"
        launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
            killall coreaudiod 2>/dev/null || true
        log "SullaLoopback driver removed."
    fi

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

SKIP_LOOPBACK=false
FROM_SOURCE=false
for arg in "$@"; do
    case "$arg" in
        --skip-loopback|--skip-blackhole) SKIP_LOOPBACK=true ;;
        --from-source) FROM_SOURCE=true ;;
    esac
done

log "Installing Audio Driver..."
echo ""

# ─── Step 1: SullaLoopback driver ────────────────────────────

if [ "$SKIP_LOOPBACK" = false ]; then
    if ! install_loopback; then
        warn "Continuing without SullaLoopback. System audio capture may not work."
    fi
    echo ""
fi

# ─── Step 2: Install binary ──────────────────────────────────

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

# ─── Step 4: coreaudiod (only if loopback driver was just installed) ─

if [ "$LOOPBACK_JUST_INSTALLED" = true ]; then
    log "Restarting coreaudiod to detect SullaLoopback..."
    launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || \
        killall coreaudiod 2>/dev/null || true

    tries=0
    while [ $tries -lt 5 ]; do
        sleep 1
        if system_profiler SPAudioDataType 2>/dev/null | grep -qi "SullaLoopback"; then
            log "SullaLoopback virtual audio device detected!"
            break
        fi
        tries=$((tries + 1))
    done
    if [ $tries -eq 5 ]; then
        warn "SullaLoopback not yet visible. A reboot may be required."
    fi
fi

# ─── Step 5: Clean up stale Multi-Output Devices ────────────────────
#
# The audio driver creates and manages the Multi-Output Device dynamically
# at runtime (AudioMirrorManager). The installer's job is only to remove
# any stale mirror left over from a previous install or crash, so the
# driver starts clean.

MULTI_OUTPUT_OK=false

cleanup_stale_mirror() {
    local swift_src="${SCRIPT_DIR}/create-multi-output.swift"
    local swift_bin="/tmp/sulla-create-multi-output"

    if [ ! -f "$swift_src" ]; then
        # No helper available — driver will handle it
        MULTI_OUTPUT_OK=true
        return 0
    fi

    # Compile the Swift helper (only used for --remove / --check)
    if ! swiftc -O "$swift_src" -o "$swift_bin" 2>&1; then
        warn "Could not compile Swift helper — driver will manage mirror at runtime"
        MULTI_OUTPUT_OK=true
        return 0
    fi

    # Remove any existing mirror so the driver creates a fresh one on startup
    # with the correct physical device and master sub-device for volume control
    if "$swift_bin" --check 2>/dev/null; then
        log "Removing stale Multi-Output Device (driver will recreate)..."
        sudo -u "$REAL_USER" "$swift_bin" --remove 2>/dev/null || true
    fi

    rm -f "$swift_bin"
    MULTI_OUTPUT_OK=true
    log "Multi-Output Device will be created by audio driver on startup."
    return 0
}

cleanup_stale_mirror
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

    # ── Unload any existing registration ──
    local service_target="gui/${REAL_UID}/${PLIST_LABEL}"

    if sudo -u "$REAL_USER" launchctl print "$service_target" &>/dev/null; then
        log "Stopping existing service..."
        sudo -u "$REAL_USER" launchctl bootout "$service_target" 2>/dev/null || true
        sleep 1
    fi

    if sudo -u "$REAL_USER" launchctl print "$service_target" &>/dev/null; then
        warn "Service label still registered after bootout — force removing..."
        sudo -u "$REAL_USER" launchctl remove "${PLIST_LABEL}" 2>/dev/null || true
        sleep 1
    fi

    # ── Load the service fresh ──
    local bootstrap_err
    if bootstrap_err=$(sudo -u "$REAL_USER" launchctl bootstrap "gui/${REAL_UID}" "$PLIST_PATH" 2>&1); then
        log "Service started (as user: ${REAL_USER})."
    else
        if echo "$bootstrap_err" | grep -qi "already"; then
            warn "Service was auto-loaded by launchd. Restarting..."
            if sudo -u "$REAL_USER" launchctl kickstart -k "$service_target" 2>/dev/null; then
                log "Service restarted (as user: ${REAL_USER})."
            else
                error "Failed to restart service."
                error "Try manually:"
                error "  launchctl bootout $service_target"
                error "  launchctl bootstrap gui/\$(id -u) ${PLIST_PATH}"
                return 1
            fi
        else
            error "Failed to start user agent: $bootstrap_err"
            error "Try manually:"
            error "  launchctl bootout $service_target"
            error "  launchctl bootstrap gui/\$(id -u) ${PLIST_PATH}"
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

if [ "$LOOPBACK_OK" = true ]; then
    log "  SullaLoopback .......... OK"
elif [ "$SKIP_LOOPBACK" = true ]; then
    warn "  SullaLoopback .......... SKIPPED"
else
    error "  SullaLoopback .......... FAILED"
    FAILURES=$((FAILURES + 1))
fi

if [ "$MULTI_OUTPUT_OK" = true ]; then
    log "  Multi-Output Device .... OK  (managed by driver)"
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
