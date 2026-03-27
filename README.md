# Audio Driver

Cross-platform audio loopback driver. Captures system audio on macOS (CoreAudio + BlackHole) and Windows (WASAPI loopback).

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
| **Local** | None | Unix socket / named pipe | Desktop app on same machine |

## Usage

```bash
# Gateway mode (standalone)
audio-driver --mode gateway --backend-url https://api.example.com --email you@example.com

# Local mode (with desktop app)
audio-driver --mode local

# List audio devices
audio-driver --list-devices
```

## Build from Source

Requires CMake 3.16+ and a C++17 compiler.

```bash
mkdir build && cd build
cmake ..
cmake --build .

# Run tests (98 tests)
./audio-driver-tests
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
docs/                Documentation
```

## Platform Status

| Feature | macOS | Windows |
|---------|-------|---------|
| Device enumeration | Done (CoreAudio) | Done (WASAPI) |
| Speaker/system capture | Done (CoreAudio + BlackHole loopback) | Done (WASAPI loopback) |
| Mic capture (input device) | **Stubbed** — needs input device backend | **Stubbed** — needs input device backend |
| Audio mirror (dynamic routing) | Done (AudioMirrorManager) | N/A (WASAPI loopback is direct) |
| Local transport (IPC) | Done (Unix socket) | **Stubbed** — needs named pipe transport |
| Gateway client (WebSocket) | **Stubbed** — needs websocketpp or similar | **Stubbed** |
| Auth client (HTTP) | **Stubbed** — needs libcurl or similar | **Stubbed** |
| Installer / service | Done (LaunchAgent + BlackHole + Multi-Output Device) | Done (install.bat) |

### What's stubbed

**Mic capture**: The driver currently only captures system/speaker audio. Mic capture requires a separate input device backend (`ICaptureBackend` for input devices rather than output loopback). The `DriverController` has the wiring in place but the TODO at line 413 marks where input device selection diverges from output device selection. In local mode, mic audio comes from the desktop app (browser `getUserMedia`), so this is only needed for standalone gateway mode.

**Gateway mode**: `IGatewayClient` and `IAuthClient` factory methods in `PlatformFactory.cpp` return nullptr. Gateway mode (driver authenticates and streams directly to the cloud without Sulla Desktop) requires WebSocket and HTTP client libraries. Local mode is the primary active path.

**Windows named pipe transport**: `ILocalTransport::create()` returns nullptr on Windows. The Unix socket transport works on macOS and Linux. Windows needs a named pipe implementation for local mode IPC with Sulla Desktop.

### What's NOT tested

- **Windows end-to-end**: WASAPI backends are implemented but have not been tested on a Windows machine. The installer (`install.bat`) exists but has not been validated.
- **Gateway mode**: Cannot be tested until the WebSocket/HTTP client stubs are implemented.

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — system design, layers, audio pipeline, concurrency model
- [Installation](docs/INSTALLATION.md) — install, uninstall, troubleshooting, service management
- [Configuration](docs/CONFIGURATION.md) — config file reference, modes, CLI overrides, device selection
- [Development](docs/DEVELOPMENT.md) — building, testing, project structure, code conventions, stubs
- [Protocol](docs/PROTOCOL.md) — local socket binary protocol, gateway WebSocket protocol, channel multiplexing

## License

Proprietary. BlackHole is licensed separately under [GPLv3](https://github.com/ExistentialAudio/BlackHole/blob/master/LICENSE).
