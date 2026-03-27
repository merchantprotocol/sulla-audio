# Wire Protocol

## Local Socket Protocol

The audio driver communicates with Sulla Desktop over a local Unix socket (`/tmp/audio-driver.sock` on macOS) using a binary frame protocol.

### Frame Format

Each frame contains one audio chunk:

```
┌──────────┬────────────────────┬──────────────────┐
│ 1 byte   │ 4 bytes            │ N bytes          │
│ source   │ payload length     │ raw PCM audio    │
│          │ (big-endian u32)   │                  │
└──────────┴────────────────────┴──────────────────┘
```

| Field | Size | Description |
|-------|------|-------------|
| source | 1 byte | `0x00` = microphone, `0x01` = speaker |
| length | 4 bytes | Payload length in bytes (big-endian uint32) |
| audio | N bytes | Raw PCM audio data |

### Audio Format

Default output format (configurable):
- Sample rate: 16,000 Hz
- Bit depth: 16-bit signed integer (little-endian)
- Channels: 1 (mono)
- Chunk interval: 200ms

At default settings, each chunk is:
- `16000 * 1 * 2 * 0.2 = 6400 bytes` of audio
- `6405 bytes` total with the 5-byte header

### Example Frame (Speaker Chunk)

```
01                  ← source: speaker
00 00 19 00         ← length: 6400 bytes (big-endian)
[6400 bytes PCM]    ← 200ms of 16kHz mono 16-bit audio
```

## Gateway WebSocket Protocol

When streaming to the enterprise gateway (either directly in gateway mode, or via Sulla Desktop in local mode), audio is sent over a WebSocket connection at `/ws/audio/{sessionId}`.

### Channel Multiplexing

Multiple audio channels (mic + speaker) are multiplexed over a single WebSocket connection.

**Channel 0 (mic):** Sent as raw binary — no header.

```
[raw PCM audio bytes]
```

**Channel 1+ (speaker, etc.):** Prefixed with a 2-byte tag:

```
┌──────────┬──────────┬──────────────────┐
│ 1 byte   │ 1 byte   │ N bytes          │
│ magic    │ channel  │ raw PCM audio    │
│ 0x01     │          │                  │
└──────────┴──────────┴──────────────────┘
```

This allows backward compatibility — receivers that don't understand multi-channel will process channel 0 audio as before.

### Session Lifecycle

1. **Create session** — `POST /api/desktop/sessions` with channel map
2. **Connect audio WebSocket** — `/ws/audio/{sessionId}`
3. **Connect listener WebSocket** — `/ws/listener/{sessionId}` (receives transcripts)
4. **Stream audio** — Binary frames on the audio WebSocket
5. **End session** — `DELETE /api/desktop/sessions/{sessionId}`

### Channel Map (Session Creation)

```json
{
  "callerName": "Sulla Secretary",
  "channels": {
    "0": { "label": "User", "source": "mic" },
    "1": { "label": "Caller", "source": "system_audio" }
  }
}
```

### Listener Events

The listener WebSocket receives JSON events:

```json
{ "event_type": "transcript_turn", "text": "...", "speaker": "User", "is_final": true }
{ "event_type": "transcript_partial", "text": "...", "speaker": "Caller" }
{ "event_type": "agent_audio", "audio": "<base64 PCM>", "format": "pcm_16k_16bit_mono" }
```

## Backpressure

The gateway WebSocket client checks `bufferedAmount` before sending. If the buffer exceeds 64KB, chunks are dropped rather than queued to prevent memory growth and latency spikes.
