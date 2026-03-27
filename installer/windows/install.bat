@echo off
REM
REM Audio Driver installer for Windows.
REM
REM Windows has native WASAPI loopback — no virtual audio device needed.
REM Default: local mode — no credentials, no gateway.
REM
REM Installs:
REM   1. audio-driver.exe  → %ProgramFiles%\AudioDriver\
REM   2. Config directory   → %APPDATA%\AudioDriver\
REM
REM Usage:
REM   install.bat
REM   install.bat --uninstall
REM

setlocal enabledelayedexpansion

set INSTALL_DIR=%ProgramFiles%\AudioDriver
set CONFIG_DIR=%APPDATA%\AudioDriver
set BINARY_NAME=audio-driver.exe
set SCRIPT_DIR=%~dp0

echo [audio-driver] Audio Driver Installer for Windows
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

REM 3. Create config directory with local mode defaults
if not exist "%CONFIG_DIR%" (
    mkdir "%CONFIG_DIR%"
    echo [audio-driver] Created config directory: %CONFIG_DIR%

    (
        echo [mode]
        echo mode=local
        echo.
        echo [auth]
        echo backend_url=
        echo email=
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
    echo [audio-driver] Default config: local mode (no credentials needed)
)

echo.
echo [audio-driver] Installation complete!
echo.
echo [audio-driver] The driver is ready in local mode — no credentials needed.
echo.
echo [audio-driver] Commands:
echo   "%INSTALL_DIR%\%BINARY_NAME%"                      Start (local mode)
echo   "%INSTALL_DIR%\%BINARY_NAME%" --list-devices       List audio devices
echo   "%INSTALL_DIR%\%BINARY_NAME%" --configure          Set up gateway mode

:end
endlocal
