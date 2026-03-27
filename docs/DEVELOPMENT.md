# Development

## Prerequisites

- C++17 compiler (clang on macOS, MSVC on Windows)
- cmake 3.16+
- GoogleTest (fetched automatically by cmake)

macOS:
```bash
xcode-select --install
brew install cmake
```

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

Targets:
- `audio-driver` — the main binary
- `audio-driver-core` — static library (models + platform factory)
- `audio-driver-tests` — unit test binary

Build without tests:
```bash
cmake .. -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build . --target audio-driver
```

## Running Tests

```bash
cd build
./audio-driver-tests
```

98 tests across 11 test files. All tests are deterministic with no platform dependencies (mocks used for device enumeration).

Test files:

| File | What it tests |
|------|--------------|
| `test_AudioFormat.cpp` | Byte calculations, time conversions, presets |
| `test_AudioBuffer.cpp` | Buffer construction, duration, frame counting |
| `test_Resampler.cpp` | Rate conversion, channel preservation, signal shape |
| `test_RingBuffer.cpp` | Lock-free SPSC: read, write, wraparound |
| `test_FormatConverter.cpp` | Float/int16, stereo/mono, round-trip |
| `test_DriverConfig.cpp` | INI serialization, validation, round-trip |
| `test_AudioChunk.cpp` | Wire format, channel tagging, duration |
| `test_AuthCredentials.cpp` | Token state, expiry, redaction |
| `test_Logger.cpp` | Credential redaction patterns |
| `test_GatewaySession.cpp` | Session validity, channel mapping, header parsing |
| `test_DeviceController.cpp` | Device selection with mock enumerator |

## Project Structure

```
sulla-audio/
├── CMakeLists.txt
├── install.sh                    # Cross-platform installer entry
├── include/sulla/                # Header-only library (public API)
│   ├── AudioFormat.h
│   ├── AudioBuffer.h
│   ├── AudioChunk.h
│   ├── ... (13 more headers)
│   └── Logger.h
├── src/
│   ├── main.cpp                  # CLI entry point
│   └── platform/
│       ├── PlatformFactory.cpp   # Compile-time platform selection
│       ├── macos/                # CoreAudio backends
│       └── windows/              # WASAPI backends
├── tests/                        # GoogleTest unit tests
├── installer/
│   └── macos/install.sh          # macOS installer (idempotent)
└── docs/                         # Documentation
```

## Code Conventions

- **Header-only models** — All value objects and utilities in `include/sulla/` are header-only. No `.cpp` files needed.
- **Interfaces** — Abstract base classes prefixed with `I` (`IDeviceEnumerator`, `ICaptureBackend`, etc.) with static `create()` factory methods.
- **Platform backends** — Implemented as headers in `src/platform/{os}/` and selected at compile time via `PlatformFactory.cpp`.
- **No exceptions in audio path** — The capture callback and ring buffer use error codes, not exceptions.
- **Lock-free audio** — `RingBuffer` uses `std::atomic` for the SPSC queue between the OS audio thread and the main thread.

## Adding a New Platform

1. Create `src/platform/{os}/` directory
2. Implement `ICaptureBackend` and `IDeviceEnumerator`
3. Add compile-time guards in `PlatformFactory.cpp`
4. Add platform libraries to `CMakeLists.txt`

## Unimplemented Stubs

The following interfaces have factory methods that return `nullptr` — they need real implementations:

| Interface | What's needed |
|-----------|--------------|
| `IGatewayClient` | WebSocket client (websocketpp or Boost.Beast) + REST client for session management |
| `IAuthClient` | HTTP client (libcurl) for email/password login and token refresh |
| `ILocalTransport` | Unix domain socket server (POSIX) / named pipe server (Windows) |

The local transport is the most critical for Sulla Desktop integration. Currently, Sulla Desktop's `AudioDriverClient` expects the socket to exist, but the driver-side server implementation is stubbed.
