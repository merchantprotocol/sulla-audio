# Configuration

## Config File Location

| Platform | Path |
|----------|------|
| macOS | `~/Library/Application Support/AudioDriver/config.ini` |
| Windows | `%APPDATA%\AudioDriver\config.ini` |
| Linux | `~/.config/audio-driver/config.ini` |

The installer creates this file with local mode defaults. It is never overwritten on re-install.

## Full Reference

```ini
[mode]
# "local" — stream via Unix socket to Sulla Desktop (no auth)
# "gateway" — stream directly to enterprise gateway (requires auth)
mode=local

[auth]
# Required for gateway mode only
backend_url=https://api.example.com
email=user@example.com
# Password and token are NEVER stored in this file.
# Password is prompted at runtime. Token is held in memory only.

[gateway]
# WebSocket gateway URL for gateway mode
url=wss://gateway.example.com

[local]
# Unix socket path (macOS/Linux) or named pipe (Windows)
socket_path=/tmp/audio-driver.sock
# Alternative: TCP port for local WebSocket (0 = use socket instead)
port=0

[audio]
# Preferred audio device ID (empty = auto-select)
# Use `audio-driver --list-devices` to see available devices
preferred_device=

# Chunk interval in milliseconds — how often audio is flushed
# Lower = less latency, higher = fewer chunks
chunk_interval_ms=200

# Target audio format for output chunks
target_sample_rate=16000
target_bit_depth=16
target_channels=1

# Which sources to capture
capture_mic=true
capture_speaker=true

# Start capturing immediately on launch
auto_start=true

[logging]
# trace, debug, info, warn, error
log_level=info
# Log detailed audio diagnostics (format, chunk sizes, timing)
log_audio_diagnostics=true
```

## Mode Details

### Local Mode (default)

No authentication. The driver creates a Unix socket at `socket_path` and waits for connections from Sulla Desktop.

Sulla Desktop's `AudioDriverClient` connects to this socket and receives labeled audio chunks (mic + speaker) via a binary frame protocol.

### Gateway Mode

Requires `backend_url` and `email` in config. Password is prompted at runtime.

The driver authenticates, obtains a JWT token, creates a session on the gateway, and streams audio directly over WebSocket. This mode is fully standalone — no Sulla Desktop required.

## CLI Overrides

All config values can be overridden via CLI flags:

```bash
audio-driver --mode gateway \
  --backend-url https://api.example.com \
  --email user@example.com \
  --gateway-url wss://gateway.example.com \
  --device "BlackHole 2ch" \
  --socket /tmp/custom.sock \
  --chunk-ms 100 \
  --log-level debug \
  --no-mic \
  --no-auto-start
```

CLI flags take precedence over the config file.

## Device Selection

Use `audio-driver --list-devices` to see available audio devices.

### macOS

On macOS, system audio capture requires a **virtual audio device** (BlackHole, Soundflower, Loopback, etc.). The driver auto-detects virtual devices by name pattern and selects the first one found.

If no virtual device is found, the driver logs an error with instructions to install BlackHole.

### Windows

All WASAPI render endpoints support native loopback capture. The driver uses the default output device unless `preferred_device` is set.
