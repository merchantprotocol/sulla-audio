#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include "AudioFormat.h"
#include "AudioBuffer.h"

namespace sulla {

/**
 * FormatConverter — pure stateless functions for PCM format conversion.
 *
 * No I/O, no allocations beyond return values, no business logic.
 * Every function is independently testable.
 */
namespace FormatConverter {

    /**
     * Convert float32 samples to int16 samples.
     * Input range: [-1.0, 1.0] → Output range: [-32768, 32767]
     */
    inline std::vector<int16_t> floatToInt16(const float* input, size_t sampleCount) {
        std::vector<int16_t> output(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
            output[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
        return output;
    }

    /**
     * Convert int16 samples to float32 samples.
     * Input range: [-32768, 32767] → Output range: [-1.0, 1.0]
     */
    inline std::vector<float> int16ToFloat(const int16_t* input, size_t sampleCount) {
        std::vector<float> output(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            output[i] = static_cast<float>(input[i]) / 32768.0f;
        }
        return output;
    }

    /**
     * Convert stereo interleaved samples to mono by averaging L+R.
     * Works on any sample type (float or int16 via template).
     */
    template<typename T>
    inline std::vector<T> stereoToMono(const T* input, size_t frameCount) {
        std::vector<T> output(frameCount);
        for (size_t i = 0; i < frameCount; ++i) {
            if constexpr (std::is_floating_point_v<T>) {
                output[i] = (input[i * 2] + input[i * 2 + 1]) * 0.5f;
            } else {
                output[i] = static_cast<T>(
                    (static_cast<int32_t>(input[i * 2]) + input[i * 2 + 1]) / 2
                );
            }
        }
        return output;
    }

    /**
     * Convert mono samples to stereo by duplicating each sample.
     */
    template<typename T>
    inline std::vector<T> monoToStereo(const T* input, size_t frameCount) {
        std::vector<T> output(frameCount * 2);
        for (size_t i = 0; i < frameCount; ++i) {
            output[i * 2]     = input[i];
            output[i * 2 + 1] = input[i];
        }
        return output;
    }

    /**
     * Convert an AudioBuffer from one format to another.
     * Handles: float↔int16, stereo↔mono conversions.
     * Returns a new buffer in the target format.
     */
    inline AudioBuffer convert(const AudioBuffer& source, AudioFormat targetFormat) {
        const AudioFormat& srcFmt = source.format();

        // Start with source data as float for uniform processing
        std::vector<float> floatData;

        if (srcFmt.isFloat && srcFmt.bitDepth == 32) {
            const float* src = source.asFloat();
            floatData.assign(src, src + (source.frameCount() * srcFmt.channels));
        } else if (!srcFmt.isFloat && srcFmt.bitDepth == 16) {
            const int16_t* src = source.asInt16();
            floatData = int16ToFloat(src, source.frameCount() * srcFmt.channels);
        }

        // Channel conversion
        uint32_t frameCount = source.frameCount();
        if (srcFmt.channels == 2 && targetFormat.channels == 1) {
            floatData = stereoToMono(floatData.data(), frameCount);
        } else if (srcFmt.channels == 1 && targetFormat.channels == 2) {
            floatData = monoToStereo(floatData.data(), frameCount);
        }

        // Output format conversion
        if (targetFormat.isFloat && targetFormat.bitDepth == 32) {
            return AudioBuffer(floatData.data(), frameCount, targetFormat);
        } else if (!targetFormat.isFloat && targetFormat.bitDepth == 16) {
            auto int16Data = floatToInt16(floatData.data(), frameCount * targetFormat.channels);
            return AudioBuffer(
                reinterpret_cast<const uint8_t*>(int16Data.data()),
                int16Data.size() * sizeof(int16_t),
                targetFormat
            );
        }

        // Fallback: return as float
        return AudioBuffer(floatData.data(), frameCount, targetFormat);
    }

} // namespace FormatConverter
} // namespace sulla
