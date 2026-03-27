#!/bin/bash
#
# SullaAudio cross-platform installer.
#
# Clones the repo to a temp directory, detects the OS,
# and runs the appropriate platform installer.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash -s -- --uninstall
#   curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash -s -- --skip-blackhole
#
# Or run locally:
#   sudo ./install.sh
#

set -e

REPO_URL="https://github.com/merchantprotocol/sulla-audio.git"
CLONE_DIR="/tmp/sulla-audio-installer"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[sulla-audio]${NC} $1"; }
error() { echo -e "${RED}[sulla-audio]${NC} $1" >&2; }

# Clone the repo
log "Downloading sulla-audio..."
rm -rf "$CLONE_DIR"
git clone --depth 1 "$REPO_URL" "$CLONE_DIR" 2>/dev/null || {
    error "Failed to clone ${REPO_URL}"
    exit 1
}

OS="$(uname -s)"

case "$OS" in
    Darwin)
        log "Detected macOS"
        exec bash "${CLONE_DIR}/installer/macos/install.sh" "$@"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        log "Detected Windows"
        exec cmd.exe /c "${CLONE_DIR}\\installer\\windows\\install.bat" "$@"
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
