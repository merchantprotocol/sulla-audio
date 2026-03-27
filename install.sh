#!/bin/bash
#
# SullaAudio cross-platform installer.
#
# Detects the OS and runs the appropriate platform installer.
#
# Usage:
#   sudo ./install.sh
#   sudo ./install.sh --uninstall
#   sudo ./install.sh --skip-blackhole   (macOS only)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[sulla-audio]${NC} $1"; }
error() { echo -e "${RED}[sulla-audio]${NC} $1" >&2; }

OS="$(uname -s)"

case "$OS" in
    Darwin)
        log "Detected macOS"
        exec bash "${SCRIPT_DIR}/installer/macos/install.sh" "$@"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        log "Detected Windows"
        # Hand off to the batch file via cmd
        exec cmd.exe /c "${SCRIPT_DIR}\\installer\\windows\\install.bat" "$@"
        ;;
    Linux)
        error "Linux is not currently supported."
        exit 1
        ;;
    *)
        error "Unknown OS: ${OS}"
        exit 1
        ;;
esac
