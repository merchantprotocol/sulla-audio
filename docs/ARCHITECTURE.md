# Architecture

## Overview

The audio driver is a C++17 application that captures system audio via platform-native APIs and streams it to consumers over a local Unix socket or enterprise WebSocket gateway.

```
┌──────────────────────────────────────────────────────────────┐
│                       main.cpp (CLI)                         │
│           Config loading · Signal handling · CLI args        │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│                    DriverController                          │
│         Top-level orchestrator · State machine               │
│     Unconfigured → Ready → Streaming → Error                 │
├──────────┬──────────────┬────────────────┬───────────────────┤
│          │              │                │                   │
│  DeviceController  CaptureController  ILocalTransport     IGatewayClient
│  Device selection  Audio pipeline     Unix socket/pipe    WebSocket stream
│                    Resample+convert                        │
│                         │                                  │
│              ┌──────────▼──────────┐                       │
│              │   ICaptureBackend   │                       │
│              │  Platform-specific  │                       │
│              └──────────┬──────────┘                       │
│         ┌───────────────┼───────────────┐                  │
│   CoreAudio (macOS)           WASAPI (Windows)             │
│   + BlackHole loopback        Native loopback              │
└────────────────────────────────────────────────────────────┘
```

## Layers

### 1. Model Layer (`include/sulla/`)

Header-only value objects and utilities with no I/O or side effects. Independently testable.

| File | Purpose |
|------|---------|
| `AudioFormat.h` | Sample rate, channels, bit depth, byte calculations |
| `AudioBuffer.h` | Owns PCM data with format metadata |
| `AudioChunk.h` | Labeled audio packet (mic/speaker) with wire format |
| `AudioDevice.h` | Device ID, name, type, loopback capability |
| `AuthCredentials.h` | Login credentials + JWT token management |
| `GatewaySession.h` | Session/call IDs, channel mapping |
| `CaptureState.h` | State machine enum + error types |
| `DriverConfig.h` | Configuration model + INI serialization |
| `RingBuffer.h` | Lock-free SPSC circular buffer |
| `Resampler.h` | Linear interpolation sample rate conversion |
| `FormatConverter.h` | PCM format conversions (float/int16, stereo/mono) |
| `Logger.h` | Structured logging with credential redaction |
| `PlatformDetector.h` | Compile-time + runtime OS detection |

### 2. Interface Layer (`include/sulla/`)

Abstract base classes defining contracts. Factory methods return platform-specific subclasses.

| Interface | Responsibility |
|-----------|---------------|
| `IDeviceEnumerator` | List and identify audio output devices |
| `ICaptureBackend` | Raw loopback audio capture from a device |
| `IGatewayClient` | REST session management + WebSocket audio streaming |
| `IAuthClient` | Email/password login, JWT token refresh |
| `ILocalTransport` | Unix socket / named pipe IPC for Sulla Desktop |

### 3. Controller Layer (`include/sulla/`)

Business logic orchestration.

| Controller | Responsibility |
|-----------|---------------|
| `DeviceController` | Selects the right capture device per platform |
| `CaptureController` | Audio pipeline: capture → resample → convert → chunk |
| `DriverController` | Top-level state machine wiring everything together |

### 4. Platform Layer (`src/platform/`)

Thin wrappers around OS-specific APIs. Minimal logic.

| File | Platform | API |
|------|----------|-----|
| `macos/CoreAudioCaptureBackend.h` | macOS | CoreAudio AudioUnit |
| `macos/CoreAudioDeviceEnumerator.h` | macOS | AudioObjectGetPropertyData |
| `windows/WasapiCaptureBackend.h` | Windows | WASAPI IAudioClient |
| `windows/WasapiDeviceEnumerator.h` | Windows | IMMDeviceEnumerator |
| `PlatformFactory.cpp` | All | Compile-time factory selection |

### 5. Application Layer (`src/main.cpp`)

CLI entry point: config persistence, password prompts, signal handling, object graph construction.

## Dual-Mode Operation

### Gateway Mode

Standalone streaming to an enterprise gateway. No dependency on Sulla Desktop.

```
audio-driver → (authenticate) → (create session) → WebSocket → Gateway
```

- Requires email + password → JWT token
- Streams both mic and speaker channels with labels
- Gateway runs STT and returns transcripts

### Local Mode

IPC streaming to Sulla Desktop, which handles gateway routing.

```
audio-driver → Unix socket (/tmp/audio-driver.sock) → Sulla Desktop
```

- No authentication required
- Binary frame protocol: `[1B source][4B length BE][NB audio]`
- Source: `0` = mic, `1` = speaker
- Sulla Desktop's `AudioDriverClient` connects and forwards to gateway

## Audio Pipeline

```
Device Output → Capture Backend (raw PCM)
    → Float conversion (uniform processing format)
    → Resample (e.g. 48kHz → 16kHz)
    → Channel conversion (stereo → mono)
    → Bit depth conversion (float → int16)
    → Ring buffer (lock-free, real-time safe)
    → Chunk assembly (200ms intervals)
    → Emit AudioChunk to consumer (socket or WebSocket)
```

## Wire Format

Audio chunks are sent as binary frames:

**Channel 0 (mic):** Raw audio bytes, no header.

**Channel 1+ (speaker):** Tagged with a 2-byte header:
```
[0x01 magic byte][channel uint8][audio bytes...]
```

This allows the receiver to demultiplex channels from a single stream.

## Concurrency Model

- **Capture callback** runs on an OS audio thread (real-time priority)
- **Ring buffer** (lock-free SPSC) bridges the callback to the main thread
- **Chunk assembly** runs on the main/controller thread
- **Socket/WebSocket I/O** runs on the main thread

The ring buffer is the only shared data structure. No mutexes in the audio path.

## Security

- Passwords are never persisted to disk or logs
- JWT tokens are sent in HTTP `Authorization` headers, never in URLs
- Logger automatically redacts: Bearer tokens, JWTs, API keys, passwords, `mpk_` prefixed keys
- Config file stores email but never password or token
