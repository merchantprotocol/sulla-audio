# Sulla Audio

Cross-platform audio loopback driver for [Sulla](https://github.com/merchantprotocol). Captures system audio on macOS (CoreAudio + BlackHole) and Windows (WASAPI loopback).

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash
```

To uninstall:

```bash
curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash -s -- --uninstall
```

## How It Works

- **macOS**: Installs [BlackHole 2ch](https://existential.audio/blackhole/) as a virtual audio device for system audio loopback via CoreAudio.
- **Windows**: Uses native WASAPI loopback capture — no additional driver needed.

## Modes

| Mode | Auth | Transport | Use Case |
|------|------|-----------|----------|
| **Gateway** | JWT (email + password) | WebSocket to enterprise gateway | Standalone deployment |
| **Local** | None | Unix socket / named pipe | Sulla Desktop on same machine |

## Usage

```bash
# Gateway mode (standalone)
sulla-audio-driver --mode gateway --backend-url https://api.example.com --email you@example.com

# Local mode (with Sulla Desktop)
sulla-audio-driver --mode local

# List audio devices
sulla-audio-driver --list-devices
```

## Build from Source

Requires CMake 3.16+ and a C++17 compiler.

```bash
mkdir build && cd build
cmake ..
cmake --build .

# Run tests (98 tests)
./sulla-audio-tests
```

## Project Structure

```
include/sulla/       Models, interfaces, utilities (header-only)
src/                 Platform backends and CLI entry point
  platform/macos/    CoreAudio device enumerator + capture backend
  platform/windows/  WASAPI device enumerator + loopback capture backend
tests/               GoogleTest unit tests
installer/           Platform-specific installers
install.sh           Cross-platform install entry point
```

## License

Proprietary. BlackHole is licensed separately under [GPLv3](https://github.com/ExistentialAudio/BlackHole/blob/master/LICENSE).
