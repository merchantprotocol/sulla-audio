#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include <fstream>
#include <cstring>
#include "ICaptureBackend.h"
#include "AudioBuffer.h"
#include "AudioFormat.h"
#include "CaptureState.h"
#include "AudioDevice.h"
#include "FormatConverter.h"
#include "Resampler.h"
#include "RingBuffer.h"
#include "GatewaySession.h"

namespace sulla {

/**
 * CaptureController — orchestrates the audio capture pipeline.
 *
 * Business decisions made here:
 *   - Whether to resample (if device format != target format)
 *   - Whether to convert channels (stereo→mono if gateway expects mono)
 *   - How to chunk audio for WebSocket delivery (by time interval)
 *   - When to start/stop based on state transitions
 *   - Which channel tag to prepend (channel 0 vs channel 1)
 *
 * Delegates raw capture to ICaptureBackend (the platform layer).
 * Delegates format math to FormatConverter and Resampler (the utility layer).
 */
class CaptureController {
public:
    /** What the controller outputs: tagged, format-converted audio chunks. */
    using ChunkCallback = std::function<void(const std::vector<uint8_t>& taggedChunk)>;
    using StateCallback = std::function<void(CaptureState state, const std::string& detail)>;

    struct Config {
        AudioFormat targetFormat = AudioFormat::telephony(); // 16kHz mono int16 for STT
        uint32_t    chunkIntervalMs = 250;                   // Chunk size for WebSocket
        uint8_t     channel = 0;                             // Channel tag (0=mic, 1=system)
    };

    CaptureController(
        std::unique_ptr<ICaptureBackend> backend,
        Config config
    )
        : backend_(std::move(backend))
        , config_(config)
        , ringBuffer_(config.targetFormat.bytesPerSecond() * 2) // 2 seconds of buffer
    {}

    /** Register callback for output audio chunks (ready for WebSocket). */
    void onChunk(ChunkCallback cb) { chunkCallback_ = std::move(cb); }

    /** Register callback for state changes. */
    void onState(StateCallback cb) { stateCallback_ = std::move(cb); }

    /** Current capture state. */
    CaptureState state() const { return state_.load(); }

    /**
     * Initialize and start capturing from the given device.
     *
     * Decisions:
     *   - Negotiates format between device native and target
     *   - Sets up resampling if sample rates differ
     *   - Sets up channel conversion if channel counts differ
     */
    CaptureError start(const AudioDevice& device) {
        setState(CaptureState::Initializing);

        auto err = backend_->initialize(device);
        if (!err.ok()) {
            setState(CaptureState::Error);
            return err;
        }

        sourceFormat_ = backend_->captureFormat();
        needsResample_ = (sourceFormat_.sampleRate != config_.targetFormat.sampleRate);
        needsChannelConvert_ = (sourceFormat_.channels != config_.targetFormat.channels);
        needsFormatConvert_ = (sourceFormat_.isFloat != config_.targetFormat.isFloat
                            || sourceFormat_.bitDepth != config_.targetFormat.bitDepth);

        // Wire up the data callback — backend delivers raw audio, we process it
        backend_->onData([this](const AudioBuffer& buffer) {
            processAudioBuffer(buffer);
        });

        backend_->onError([this](const CaptureError& error) {
            setState(CaptureState::Error);
        });

        err = backend_->start();
        if (!err.ok()) {
            setState(CaptureState::Error);
            return err;
        }

        openDebugWav();

        setState(CaptureState::Capturing);
        return CaptureError::none();
    }

    /** Stop capturing and release resources. */
    void stop() {
        setState(CaptureState::Stopping);
        backend_->stop();
        flushRingBuffer();
        closeDebugWav();
        backend_->shutdown();
        setState(CaptureState::Idle);
    }

private:
    std::unique_ptr<ICaptureBackend> backend_;
    Config              config_;
    RingBuffer          ringBuffer_;
    ChunkCallback       chunkCallback_;
    StateCallback       stateCallback_;
    std::atomic<CaptureState> state_{CaptureState::Idle};

    AudioFormat sourceFormat_;
    bool needsResample_       = false;
    bool needsChannelConvert_ = false;
    bool needsFormatConvert_  = false;

    // Debug WAV recording
    std::ofstream debugWav_;
    uint32_t debugWavDataBytes_ = 0;

    void openDebugWav() {
        std::string path = "/tmp/audio-driver-ch" + std::to_string(config_.channel) + ".wav";
        debugWav_.open(path, std::ios::binary | std::ios::trunc);
        if (!debugWav_.is_open()) return;

        // Write placeholder WAV header (44 bytes) — finalized on stop()
        const auto& fmt = config_.targetFormat;
        uint16_t audioFmt   = fmt.isFloat ? 3 : 1; // 3=IEEE float, 1=PCM
        uint16_t numCh      = fmt.channels;
        uint32_t rate       = fmt.sampleRate;
        uint16_t bitsPerSmp = fmt.bitDepth;
        uint16_t blockAlign = numCh * (bitsPerSmp / 8);
        uint32_t byteRate   = rate * blockAlign;

        char header[44];
        std::memset(header, 0, 44);
        std::memcpy(header,      "RIFF", 4);
        // header[4..7] = file size - 8 (filled on close)
        std::memcpy(header + 8,  "WAVE", 4);
        std::memcpy(header + 12, "fmt ", 4);
        uint32_t fmtSize = 16;
        std::memcpy(header + 16, &fmtSize,    4);
        std::memcpy(header + 20, &audioFmt,   2);
        std::memcpy(header + 22, &numCh,      2);
        std::memcpy(header + 24, &rate,        4);
        std::memcpy(header + 28, &byteRate,    4);
        std::memcpy(header + 32, &blockAlign,  2);
        std::memcpy(header + 34, &bitsPerSmp,  2);
        std::memcpy(header + 36, "data", 4);
        // header[40..43] = data size (filled on close)

        debugWav_.write(header, 44);
        debugWavDataBytes_ = 0;
    }

    void writeDebugWav(const uint8_t* data, size_t len) {
        if (!debugWav_.is_open()) return;
        debugWav_.write(reinterpret_cast<const char*>(data), len);
        debugWavDataBytes_ += static_cast<uint32_t>(len);
    }

    void closeDebugWav() {
        if (!debugWav_.is_open()) return;

        // Patch RIFF size (file size - 8)
        uint32_t riffSize = debugWavDataBytes_ + 36;
        debugWav_.seekp(4);
        debugWav_.write(reinterpret_cast<const char*>(&riffSize), 4);

        // Patch data chunk size
        debugWav_.seekp(40);
        debugWav_.write(reinterpret_cast<const char*>(&debugWavDataBytes_), 4);

        debugWav_.close();
    }

    /**
     * Core processing pipeline — called from the capture thread.
     *
     * Decision chain:
     *   1. Convert to float if not already (for uniform processing)
     *   2. Resample if sample rates differ
     *   3. Convert channels if needed
     *   4. Convert to target bit depth/type
     *   5. Write to ring buffer
     *   6. Flush chunks when enough data accumulated
     */
    void processAudioBuffer(const AudioBuffer& buffer) {
        // Step 1: Get float representation
        std::vector<float> floatData;
        if (sourceFormat_.isFloat && sourceFormat_.bitDepth == 32) {
            const float* src = buffer.asFloat();
            floatData.assign(src, src + buffer.frameCount() * sourceFormat_.channels);
        } else if (!sourceFormat_.isFloat && sourceFormat_.bitDepth == 16) {
            const int16_t* src = buffer.asInt16();
            floatData = FormatConverter::int16ToFloat(src, buffer.frameCount() * sourceFormat_.channels);
        }

        uint32_t frames = buffer.frameCount();
        uint16_t channels = sourceFormat_.channels;

        // Step 2: Resample
        if (needsResample_) {
            floatData = Resampler::resample(
                floatData.data(), frames, channels,
                sourceFormat_.sampleRate, config_.targetFormat.sampleRate
            );
            frames = static_cast<uint32_t>(floatData.size() / channels);
        }

        // Step 3: Channel conversion
        if (needsChannelConvert_) {
            if (channels == 2 && config_.targetFormat.channels == 1) {
                floatData = FormatConverter::stereoToMono(floatData.data(), frames);
                channels = 1;
            } else if (channels == 1 && config_.targetFormat.channels == 2) {
                floatData = FormatConverter::monoToStereo(floatData.data(), frames);
                channels = 2;
            }
        }

        // Step 4: Convert to target format and write to ring buffer
        if (!config_.targetFormat.isFloat && config_.targetFormat.bitDepth == 16) {
            auto int16Data = FormatConverter::floatToInt16(floatData.data(), floatData.size());
            ringBuffer_.write(
                reinterpret_cast<const uint8_t*>(int16Data.data()),
                int16Data.size() * sizeof(int16_t)
            );
        } else {
            ringBuffer_.write(
                reinterpret_cast<const uint8_t*>(floatData.data()),
                floatData.size() * sizeof(float)
            );
        }

        // Step 5: Flush chunks
        flushRingBuffer();
    }

    /** Emit chunks from the ring buffer when enough data has accumulated. */
    void flushRingBuffer() {
        const size_t chunkBytes = static_cast<size_t>(
            config_.targetFormat.bytesPerSecond() * config_.chunkIntervalMs / 1000
        );

        while (ringBuffer_.availableRead() >= chunkBytes) {
            std::vector<uint8_t> raw(chunkBytes);
            ringBuffer_.read(raw.data(), chunkBytes);

            writeDebugWav(raw.data(), raw.size());

            if (chunkCallback_) {
                chunkCallback_(raw);
            }
        }
    }

    void setState(CaptureState newState) {
        state_.store(newState);
        if (stateCallback_) {
            stateCallback_(newState, captureStateToString(newState));
        }
    }
};

} // namespace sulla
