#!/bin/bash
#
# AudioDriver cross-platform installer.
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
CLONE_DIR="/tmp/.audio-driver"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[audio-driver]${NC} $1"; }
error() { echo -e "${RED}[audio-driver]${NC} $1" >&2; }

# Clone or update the repo (close stdin so git doesn't hang when piped from curl)
# Always do a fresh shallow clone to avoid stale code, shallow-repo pull
# failures, and permission mismatches from previous installs.
log "Downloading audio-driver..."
rm -rf "$CLONE_DIR"
if ! git clone --depth 1 "$REPO_URL" "$CLONE_DIR" </dev/null 2>&1; then
    error "Failed to clone repository from ${REPO_URL}"
    exit 1
fi
if [ ! -d "$CLONE_DIR/installer" ]; then
    error "Clone succeeded but installer directory is missing."
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

log "Done. Temporary files at ${CLONE_DIR} can be safely deleted."
