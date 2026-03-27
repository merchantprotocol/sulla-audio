#pragma once

#include <cstdint>
#include <vector>
#include <cmath>

namespace sulla {

/**
 * Resampler — sample rate conversion using linear interpolation.
 *
 * Pure utility. Stateless per call. No I/O, no platform deps.
 * For production quality, swap the interpolation for a polyphase
 * filter (libsamplerate), but linear is correct and testable.
 */
namespace Resampler {

    /**
     * Resample a block of float samples from one rate to another.
     *
     * @param input       Source samples (interleaved if multi-channel)
     * @param frameCount  Number of frames in the source
     * @param channels    Number of interleaved channels
     * @param srcRate     Source sample rate
     * @param dstRate     Target sample rate
     * @return            Resampled frames (interleaved, same channel count)
     */
    inline std::vector<float> resample(
        const float* input,
        uint32_t frameCount,
        uint16_t channels,
        uint32_t srcRate,
        uint32_t dstRate
    ) {
        if (srcRate == dstRate || frameCount == 0) {
            return std::vector<float>(input, input + frameCount * channels);
        }

        const double ratio = static_cast<double>(dstRate) / srcRate;
        const uint32_t outFrames = static_cast<uint32_t>(std::ceil(frameCount * ratio));
        std::vector<float> output(outFrames * channels);

        for (uint32_t i = 0; i < outFrames; ++i) {
            const double srcPos = i / ratio;
            const uint32_t srcIdx = static_cast<uint32_t>(srcPos);
            const float frac = static_cast<float>(srcPos - srcIdx);

            const uint32_t idx0 = srcIdx;
            const uint32_t idx1 = (srcIdx + 1 < frameCount) ? srcIdx + 1 : srcIdx;

            for (uint16_t ch = 0; ch < channels; ++ch) {
                const float s0 = input[idx0 * channels + ch];
                const float s1 = input[idx1 * channels + ch];
                output[i * channels + ch] = s0 + frac * (s1 - s0);
            }
        }

        return output;
    }

    /**
     * Calculate the output frame count for a given resample operation.
     */
    inline uint32_t outputFrameCount(uint32_t inputFrames, uint32_t srcRate, uint32_t dstRate) {
        if (srcRate == dstRate) return inputFrames;
        return static_cast<uint32_t>(std::ceil(
            static_cast<double>(inputFrames) * dstRate / srcRate
        ));
    }

} // namespace Resampler
} // namespace sulla
