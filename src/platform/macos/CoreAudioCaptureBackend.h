#pragma once

#ifdef __APPLE__

#include <sulla/ICaptureBackend.h>
#include <sulla/AudioBuffer.h>
#include <sulla/AudioFormat.h>
#include <sulla/CaptureState.h>

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <string>

namespace sulla {

/**
 * CoreAudioCaptureBackend — captures audio from a virtual input device on macOS.
 *
 * On macOS, loopback capture requires a virtual audio device (BlackHole, SullaAudio)
 * that routes system output to an input tap. This backend opens that device's input
 * stream and delivers raw PCM buffers via the data callback.
 *
 * Thin wrapper. No business decisions — just reads from the device.
 */
class CoreAudioCaptureBackend : public ICaptureBackend {
public:
    ~CoreAudioCaptureBackend() override { shutdown(); }

    CaptureError initialize(const AudioDevice& device) override {
        deviceId_ = static_cast<AudioDeviceID>(std::stoul(device.id));
        format_ = device.nativeFormat;

        // Set up AudioUnit for input from the virtual device
        AudioComponentDescription desc = {
            kAudioUnitType_Output,
            kAudioUnitSubType_HALOutput,
            kAudioUnitManufacturer_Apple,
            0, 0
        };

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "HALOutput AudioUnit not found");
        }

        OSStatus status = AudioComponentInstanceNew(component, &audioUnit_);
        if (status != noErr) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Failed to create AudioUnit", status);
        }

        // Enable input, disable output
        UInt32 enableIO = 1;
        status = AudioUnitSetProperty(audioUnit_,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Input, 1,
            &enableIO, sizeof(enableIO));

        enableIO = 0;
        status = AudioUnitSetProperty(audioUnit_,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Output, 0,
            &enableIO, sizeof(enableIO));

        // Set the input device
        status = AudioUnitSetProperty(audioUnit_,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0,
            &deviceId_, sizeof(deviceId_));
        if (status != noErr) {
            return CaptureError::make(CaptureError::Code::DeviceNotFound,
                "Failed to set input device", status);
        }

        // Set up stream format (request float32)
        AudioStreamBasicDescription streamFormat = {};
        streamFormat.mSampleRate       = format_.sampleRate;
        streamFormat.mFormatID         = kAudioFormatLinearPCM;
        streamFormat.mFormatFlags      = kAudioFormatFlagIsFloat
                                       | kAudioFormatFlagIsPacked
                                       | kAudioFormatFlagIsNonInterleaved;
        streamFormat.mBitsPerChannel   = 32;
        streamFormat.mChannelsPerFrame = format_.channels;
        streamFormat.mFramesPerPacket  = 1;
        streamFormat.mBytesPerFrame    = 4;
        streamFormat.mBytesPerPacket   = 4;

        status = AudioUnitSetProperty(audioUnit_,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Output, 1,
            &streamFormat, sizeof(streamFormat));

        // Set input callback
        AURenderCallbackStruct callbackStruct = {
            inputCallback, this
        };
        status = AudioUnitSetProperty(audioUnit_,
            kAudioOutputUnitProperty_SetInputCallback,
            kAudioUnitScope_Global, 0,
            &callbackStruct, sizeof(callbackStruct));

        status = AudioUnitInitialize(audioUnit_);
        if (status != noErr) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Failed to initialize AudioUnit", status);
        }

        return CaptureError::none();
    }

    CaptureError start() override {
        if (!audioUnit_) {
            return CaptureError::make(CaptureError::Code::InitializationFailed,
                "Backend not initialized");
        }

        running_.store(true);
        OSStatus status = AudioOutputUnitStart(audioUnit_);
        if (status != noErr) {
            return CaptureError::make(CaptureError::Code::CaptureStartFailed,
                "Failed to start AudioUnit", status);
        }
        return CaptureError::none();
    }

    CaptureError stop() override {
        running_.store(false);
        if (audioUnit_) {
            AudioOutputUnitStop(audioUnit_);
        }
        return CaptureError::none();
    }

    void shutdown() override {
        stop();
        if (audioUnit_) {
            AudioUnitUninitialize(audioUnit_);
            AudioComponentInstanceDispose(audioUnit_);
            audioUnit_ = nullptr;
        }
    }

    AudioFormat captureFormat() const override { return format_; }

private:
    AudioDeviceID     deviceId_  = 0;
    AudioUnit         audioUnit_ = nullptr;
    AudioFormat       format_;
    std::atomic<bool> running_{false};

    /**
     * CoreAudio input callback — called from the audio thread.
     * Renders input from the virtual device and forwards to the data callback.
     */
    static OSStatus inputCallback(
        void* inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp* inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList* /*ioData*/
    ) {
        auto* self = static_cast<CoreAudioCaptureBackend*>(inRefCon);
        if (!self->running_.load()) return noErr;

        // Allocate buffer list for render
        const uint16_t channels = self->format_.channels;
        const size_t bufferSize = inNumberFrames * sizeof(float);

        // For non-interleaved, we need one buffer per channel
        std::vector<uint8_t> storage(sizeof(AudioBufferList) + (channels - 1) * sizeof(AudioBuffer));
        auto* bufferList = reinterpret_cast<AudioBufferList*>(storage.data());
        bufferList->mNumberBuffers = channels;

        std::vector<std::vector<float>> channelBuffers(channels, std::vector<float>(inNumberFrames));
        for (uint16_t ch = 0; ch < channels; ++ch) {
            bufferList->mBuffers[ch].mNumberChannels = 1;
            bufferList->mBuffers[ch].mDataByteSize = static_cast<UInt32>(bufferSize);
            bufferList->mBuffers[ch].mData = channelBuffers[ch].data();
        }

        OSStatus status = AudioUnitRender(
            self->audioUnit_, ioActionFlags, inTimeStamp, inBusNumber,
            inNumberFrames, bufferList
        );
        if (status != noErr) return status;

        // Interleave channels into a single buffer
        std::vector<float> interleaved(inNumberFrames * channels);
        for (uint32_t frame = 0; frame < inNumberFrames; ++frame) {
            for (uint16_t ch = 0; ch < channels; ++ch) {
                interleaved[frame * channels + ch] = channelBuffers[ch][frame];
            }
        }

        AudioBuffer audioBuffer(interleaved.data(), inNumberFrames, self->format_);
        self->emitData(audioBuffer);

        return noErr;
    }
};

} // namespace sulla

#endif // __APPLE__
