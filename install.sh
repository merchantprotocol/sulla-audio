#!/bin/bash
#
# SullaAudio cross-platform installer.
#
# Detects the OS and runs the appropriate platform installer.
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

REPO_RAW="https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[sulla-audio]${NC} $1"; }
error() { echo -e "${RED}[sulla-audio]${NC} $1" >&2; }

fetch_and_run() {
    local url="$1"
    shift
    local script
    script=$(curl -fsSL "$url") || {
        error "Failed to download: ${url}"
        exit 1
    }
    bash -c "$script" -- "$@"
}

OS="$(uname -s)"

case "$OS" in
    Darwin)
        log "Detected macOS"
        fetch_and_run "${REPO_RAW}/installer/macos/install.sh" "$@"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        log "Detected Windows"
        fetch_and_run "${REPO_RAW}/installer/windows/install.sh" "$@"
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
