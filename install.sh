#!/bin/bash
#
# AudioDriver cross-platform installer.
#
# Clones the repo to a temp directory, detects the OS,
# and runs the appropriate platform installer.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/audio-driver/main/install.sh | sudo bash
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/audio-driver/main/install.sh | sudo bash -s -- --uninstall
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/audio-driver/main/install.sh | sudo bash -s -- --skip-blackhole
#
# Or run locally:
#   sudo ./install.sh
#

set -e

REPO_URL="https://github.com/merchantprotocol/sulla-audio.git"
CLONE_DIR="/tmp/.audio-driver"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[audio-driver]${NC} $1"; }
error() { echo -e "${RED}[audio-driver]${NC} $1" >&2; }

# Clone or update the repo (close stdin so git doesn't hang when piped from curl)
log "Downloading audio-driver..."
if [ -d "$CLONE_DIR/.git" ]; then
    git -C "$CLONE_DIR" pull --ff-only </dev/null 2>/dev/null || true
else
    rm -rf "$CLONE_DIR"
    git clone --depth 1 "$REPO_URL" "$CLONE_DIR" </dev/null
fi
if [ ! -d "$CLONE_DIR/installer" ]; then
    error "Failed to clone repository."
    exit 1
fi
log "Downloaded to ${CLONE_DIR}"

OS="$(uname -s)"

case "$OS" in
    Darwin)
        log "Detected macOS"
        bash "${CLONE_DIR}/installer/macos/install.sh" "$@"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        log "Detected Windows"
        cmd.exe /c "${CLONE_DIR}\\installer\\windows\\install.bat" "$@"
        ;;
    Linux)
        error "Linux is not currently supported."
        rm -rf "$CLONE_DIR"
        exit 1
        ;;
    *)
        error "Unknown OS: ${OS}"
        rm -rf "$CLONE_DIR"
        exit 1
        ;;
esac

log "Installed to ${CLONE_DIR}"
