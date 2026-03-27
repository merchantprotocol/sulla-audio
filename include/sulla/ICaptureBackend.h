#pragma once

#include <functional>
#include <memory>
#include "AudioBuffer.h"
#include "AudioDevice.h"
#include "CaptureState.h"

namespace sulla {

/**
 * ICaptureBackend — interface for raw loopback audio capture.
 *
 * Platform backends implement this. The backend's only job is:
 *   1. Open a loopback stream on a given device
 *   2. Call the onData callback with raw audio buffers
 *   3. Report errors via onError
 *
 * No decisions about format conversion, resampling, gateway connection,
 * or anything else — that's the controller's job.
 */
class ICaptureBackend {
public:
    using DataCallback  = std::function<void(const AudioBuffer& buffer)>;
    using ErrorCallback = std::function<void(const CaptureError& error)>;

    virtual ~ICaptureBackend() = default;

    /** Initialize the backend for a specific device. */
    virtual CaptureError initialize(const AudioDevice& device) = 0;

    /** Start capturing. Calls onData from the capture thread. */
    virtual CaptureError start() = 0;

    /** Stop capturing. */
    virtual CaptureError stop() = 0;

    /** Release all resources. */
    virtual void shutdown() = 0;

    /** The native audio format the backend will deliver. */
    virtual AudioFormat captureFormat() const = 0;

    /** Register the data callback (called from capture thread). */
    void onData(DataCallback cb) { dataCallback_ = std::move(cb); }

    /** Register the error callback. */
    void onError(ErrorCallback cb) { errorCallback_ = std::move(cb); }

    /** Factory: create the platform-appropriate capture backend. */
    static std::unique_ptr<ICaptureBackend> create();

protected:
    DataCallback  dataCallback_;
    ErrorCallback errorCallback_;

    void emitData(const AudioBuffer& buffer) {
        if (dataCallback_) dataCallback_(buffer);
    }

    void emitError(const CaptureError& error) {
        if (errorCallback_) errorCallback_(error);
    }
};

} // namespace sulla
