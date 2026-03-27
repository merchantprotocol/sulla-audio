@echo off
REM
REM AudioDriver driver installer for Windows.
REM
REM Windows has native WASAPI loopback support — no virtual audio device needed.
REM This installer just copies the driver binary and creates the config directory.
REM
REM Installs:
REM   1. audio-driver-driver.exe  → %ProgramFiles%\AudioDriver\
REM   2. Config directory        → %APPDATA%\AudioDriver\
REM
REM Usage:
REM   install.bat
REM   install.bat --uninstall
REM

setlocal enabledelayedexpansion

set INSTALL_DIR=%ProgramFiles%\AudioDriver
set CONFIG_DIR=%APPDATA%\AudioDriver
set BINARY_NAME=audio-driver-driver.exe
set SCRIPT_DIR=%~dp0

echo [audio-driver] AudioDriver Driver Installer for Windows
echo.

REM ── Uninstall ──────────────────────────────────────────────

if "%1"=="--uninstall" (
    echo [audio-driver] Uninstalling...

    if exist "%INSTALL_DIR%\%BINARY_NAME%" (
        del /f "%INSTALL_DIR%\%BINARY_NAME%"
        echo [audio-driver] Removed %INSTALL_DIR%\%BINARY_NAME%
    )
    if exist "%INSTALL_DIR%" (
        rmdir "%INSTALL_DIR%" 2>nul
    )

    echo [audio-driver] Uninstall complete.
    echo [audio-driver] Config preserved at: %CONFIG_DIR%
    echo [audio-driver] To remove: rmdir /s /q "%CONFIG_DIR%"
    goto :end
)

REM ── Install ────────────────────────────────────────────────

echo [audio-driver] Installing...
echo [audio-driver] NOTE: Windows has native WASAPI loopback — no virtual driver needed.
echo.

REM 1. Create install directory
if not exist "%INSTALL_DIR%" (
    mkdir "%INSTALL_DIR%"
    echo [audio-driver] Created %INSTALL_DIR%
)

REM 2. Copy binary
set BINARY_SOURCE=%SCRIPT_DIR%..\..\build\Release\%BINARY_NAME%
if exist "%BINARY_SOURCE%" (
    copy /y "%BINARY_SOURCE%" "%INSTALL_DIR%\%BINARY_NAME%" >nul
    echo [audio-driver] Installed %INSTALL_DIR%\%BINARY_NAME%
) else (
    echo [audio-driver] WARNING: Binary not found at %BINARY_SOURCE%
    echo [audio-driver] Build first: cmake --build build --config Release
)

REM 3. Create config directory
if not exist "%CONFIG_DIR%" (
    mkdir "%CONFIG_DIR%"
    echo [audio-driver] Created config directory: %CONFIG_DIR%

    REM Write default config
    (
        echo [mode]
        echo mode=gateway
        echo.
        echo [auth]
        echo backend_url=
        echo email=
        echo # password is never saved — enter at runtime
        echo.
        echo [gateway]
        echo url=
        echo.
        echo [local]
        echo socket_path=
        echo port=0
        echo.
        echo [audio]
        echo preferred_device=
        echo chunk_interval_ms=200
        echo target_sample_rate=16000
        echo target_bit_depth=16
        echo target_channels=1
        echo capture_mic=true
        echo capture_speaker=true
        echo auto_start=true
        echo.
        echo [logging]
        echo log_level=info
        echo log_audio_diagnostics=true
    ) > "%CONFIG_DIR%\config.ini"
    echo [audio-driver] Default config written to %CONFIG_DIR%\config.ini
)

echo.
echo [audio-driver] Installation complete!
echo.
echo [audio-driver] Next steps:
echo.
echo   Gateway mode (standalone):
echo     "%INSTALL_DIR%\%BINARY_NAME%" --mode gateway --backend-url https://api.example.com --email you@example.com
echo.
echo   Local mode (with Sulla Desktop):
echo     "%INSTALL_DIR%\%BINARY_NAME%" --mode local
echo.
echo   List devices:
echo     "%INSTALL_DIR%\%BINARY_NAME%" --list-devices

:end
endlocal
