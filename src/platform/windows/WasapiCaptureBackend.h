#pragma once

#ifdef _WIN32

#include <sulla/ICaptureBackend.h>
#include <sulla/AudioBuffer.h>
#include <sulla/AudioFormat.h>
#include <sulla/CaptureState.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace sulla {

/**
 * WasapiCaptureBackend — raw loopback capture via Windows WASAPI.
 *
 * Thin wrapper. Opens AUDCLNT_STREAMFLAGS_LOOPBACK on a render endpoint
 * and delivers raw float32 PCM buffers via the data callback.
 *
 * No format conversion, no resampling, no gateway logic.
 * The controller decides what to do with the data.
 */
class WasapiCaptureBackend : public ICaptureBackend {
public:
    ~WasapiCaptureBackend() override { shutdown(); }

    CaptureError initialize(const AudioDevice& device) override {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // Get the MMDevice by ID
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator)
        );
        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::PlatformError,
                "Failed to create device enumerator", hr);
        }

        if (device.id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        } else {
            std::wstring wideId(device.id.begin(), device.id.end());
            hr = enumerator->GetDevice(wideId.c_str(), &device_);
        }
        enumerator->Release();

        if (FAILED(hr) || !device_) {
            return CaptureError::make(CaptureError::Code::DeviceNotFound,
                "Device not found: " + device.name, hr);
        }

        // Activate audio client
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Failed to activate audio client", hr);
        }

        // Get mix format (native device format)
        WAVEFORMATEX* mixFormat = nullptr;
        hr = audioClient_->GetMixFormat(&mixFormat);
        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::FormatNotSupported,
                "Failed to get mix format", hr);
        }

        format_ = AudioFormat{
            mixFormat->nSamplesPerSec,
            static_cast<uint16_t>(mixFormat->nChannels),
            static_cast<uint16_t>(mixFormat->wBitsPerSample),
            true  // WASAPI shared mode is always float32
        };

        // Initialize in shared loopback mode (1 second buffer)
        const REFERENCE_TIME bufferDuration = 10000000; // 100ns units = 1 second
        hr = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            mixFormat,
            nullptr
        );
        CoTaskMemFree(mixFormat);

        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Failed to initialize loopback capture", hr);
        }

        // Get capture client
        hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient_));
        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Failed to get capture client", hr);
        }

        return CaptureError::none();
    }

    CaptureError start() override {
        if (!audioClient_ || !captureClient_) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Backend not initialized");
        }

        HRESULT hr = audioClient_->Start();
        if (FAILED(hr)) {
            return CaptureError::make(CaptureError::Code::CaptureStartFailed,
                "Failed to start capture", hr);
        }

        running_.store(true);
        captureThread_ = std::thread([this]() { captureLoop(); });

        return CaptureError::none();
    }

    CaptureError stop() override {
        running_.store(false);
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
        if (audioClient_) {
            audioClient_->Stop();
        }
        return CaptureError::none();
    }

    void shutdown() override {
        stop();
        if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
        if (audioClient_)   { audioClient_->Release();   audioClient_ = nullptr; }
        if (device_)        { device_->Release();        device_ = nullptr; }
    }

    AudioFormat captureFormat() const override { return format_; }

private:
    IMMDevice*           device_        = nullptr;
    IAudioClient*        audioClient_   = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    AudioFormat          format_;
    std::atomic<bool>    running_{false};
    std::thread          captureThread_;

    void captureLoop() {
        while (running_.load()) {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                emitError(CaptureError::make(CaptureError::Code::DeviceLost,
                    "GetNextPacketSize failed", hr));
                break;
            }

            while (packetLength > 0) {
                BYTE* data = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;

                hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFrames > 0) {
                    AudioBuffer buffer(
                        reinterpret_cast<const uint8_t*>(data),
                        numFrames * format_.bytesPerFrame(),
                        format_
                    );
                    emitData(buffer);
                }

                captureClient_->ReleaseBuffer(numFrames);
                captureClient_->GetNextPacketSize(&packetLength);
            }

            // Sleep half the buffer period to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

} // namespace sulla

#endif // _WIN32
