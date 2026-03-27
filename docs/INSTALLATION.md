# Installation

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash
```

This will:
1. Clone the repo to `/tmp/.audio-driver`
2. Detect your OS and run the platform installer
3. Install BlackHole 2ch (macOS only, for system audio loopback)
4. Build the `audio-driver` binary from source
5. Install it to `/usr/local/bin/audio-driver`
6. Create a default config at `~/Library/Application Support/AudioDriver/config.ini`
7. Register and start a launchd service (auto-starts on boot)
8. Verify the socket is live at `/tmp/audio-driver.sock`

## Requirements

### macOS

- **cmake** — installed automatically via Homebrew if missing
- **BlackHole 2ch** — installed automatically (Homebrew or direct download)
- **Xcode Command Line Tools** — for the C++ compiler

### Windows

- **cmake** — must be installed manually
- **Visual Studio** or **MSVC Build Tools** — for the C++ compiler
- No additional audio driver needed (WASAPI has native loopback)

## Re-running the Installer

The installer is **idempotent**. Run it again at any time to fix a broken state:

```bash
curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash
```

On re-run it will:
- Skip BlackHole if already installed (detects via pkgutil, HAL plugin dir, and system_profiler)
- Clean up stale Homebrew receipts if BlackHole was manually deleted
- Skip the build if the binary is already up to date (`cmp` check)
- Skip config creation if `config.ini` already exists (won't overwrite your changes)
- Only restart the launchd service if it's not already running with a live socket
- Remove stale socket files if the process behind them is dead

## Uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/merchantprotocol/sulla-audio/main/install.sh | sudo bash -s -- --uninstall
```

Or locally:

```bash
sudo ./install.sh --uninstall
```

This removes the binary, launchd service, and socket. Config and BlackHole are preserved (other apps may depend on BlackHole).

To fully remove BlackHole:

```bash
brew uninstall --cask blackhole-2ch
# — or —
sudo rm -rf /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver
sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
```

To remove config:

```bash
rm -rf ~/Library/Application\ Support/AudioDriver
```

## Skip BlackHole

If you don't need system audio capture or want to install BlackHole separately:

```bash
sudo ./install.sh --skip-blackhole
```

## Build from Source (Manual)

```bash
git clone https://github.com/merchantprotocol/sulla-audio.git
cd sulla-audio
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target audio-driver
```

The binary will be at `build/audio-driver`.

## Verify Installation

```bash
# Check the binary
which audio-driver
audio-driver --list-devices

# Check the service
sudo launchctl print system/com.audiodriver.agent

# Check the socket
ls -la /tmp/audio-driver.sock

# Check logs
cat /tmp/audio-driver.log
```

## Service Management

```bash
# Restart
sudo launchctl kickstart -k system/com.audiodriver.agent

# Stop
sudo launchctl bootout system/com.audiodriver.agent

# Start (after stop)
sudo launchctl bootstrap system /Library/LaunchDaemons/com.audiodriver.agent.plist

# View logs
cat /tmp/audio-driver.log
tail -f /tmp/audio-driver.log
```

## Troubleshooting

### Socket doesn't appear after install

Check the logs:
```bash
cat /tmp/audio-driver.log
```

Common causes:
- BlackHole not installed or not visible to CoreAudio — try rebooting
- Binary failed to build — re-run the installer and check cmake output
- Another process holding the socket — `lsof /tmp/audio-driver.sock`

### BlackHole installed but not detected

The installer checks three places:
1. `pkgutil --pkg-info audio.existential.BlackHole2ch`
2. `/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver` directory
3. `system_profiler SPAudioDataType` output

If none match, try restarting CoreAudio:
```bash
sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
```

If that doesn't work, reboot. macOS sometimes caches the audio device list.

### Brew says BlackHole is installed but it's not

This happens when BlackHole files are manually deleted without `brew uninstall`. The installer detects this and cleans up the stale Homebrew receipt automatically on the next run.

### Build fails

Ensure you have:
```bash
xcode-select --install   # Xcode CLI tools
brew install cmake        # cmake
```

Then re-run the installer.

### "Binary not available" warning

The build step failed silently in a previous run. The installer now shows full cmake output. Re-run it and check for compiler errors.
