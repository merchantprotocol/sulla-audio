#include <sulla/IDeviceEnumerator.h>
#include <sulla/ICaptureBackend.h>
#include <sulla/IGatewayClient.h>
#include <sulla/IAuthClient.h>
#include <sulla/ILocalTransport.h>

#ifdef _WIN32
#include "windows/WasapiDeviceEnumerator.h"
#include "windows/WasapiCaptureBackend.h"
#elif defined(__APPLE__)
#include "macos/CoreAudioDeviceEnumerator.h"
#include "macos/CoreAudioCaptureBackend.h"
#endif

#if defined(__APPLE__) || defined(__linux__)
#include "UnixSocketTransport.h"
#endif

namespace sulla {

/**
 * Factory methods — select the right platform implementation at compile time.
 * No runtime decisions, no business logic.
 */

std::unique_ptr<IDeviceEnumerator> IDeviceEnumerator::create() {
#ifdef _WIN32
    return std::make_unique<WasapiDeviceEnumerator>();
#elif defined(__APPLE__)
    return std::make_unique<CoreAudioDeviceEnumerator>();
#else
    return nullptr;
#endif
}

std::unique_ptr<ICaptureBackend> ICaptureBackend::create() {
#ifdef _WIN32
    return std::make_unique<WasapiCaptureBackend>();
#elif defined(__APPLE__)
    return std::make_unique<CoreAudioCaptureBackend>();
#else
    return nullptr;
#endif
}

// ─── Stub factories for interfaces not yet implemented ────────
// These return nullptr — real implementations will be added when
// the HTTP (libcurl), WebSocket (websocketpp), and IPC backends are built.

std::unique_ptr<IGatewayClient> IGatewayClient::create() {
    // TODO: implement WebSocket-based gateway client
    return nullptr;
}

std::unique_ptr<IAuthClient> IAuthClient::create() {
    // TODO: implement HTTP-based auth client (libcurl)
    return nullptr;
}

std::unique_ptr<ILocalTransport> ILocalTransport::create() {
#if defined(__APPLE__) || defined(__linux__)
    return std::make_unique<UnixSocketTransport>();
#else
    // TODO: implement named pipe transport for Windows
    return nullptr;
#endif
}

} // namespace sulla
