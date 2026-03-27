#pragma once

#include <memory>
#include <functional>
#include <atomic>
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

        setState(CaptureState::Capturing);
        return CaptureError::none();
    }

    /** Stop capturing and release resources. */
    void stop() {
        setState(CaptureState::Stopping);
        backend_->stop();
        flushRingBuffer();
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
