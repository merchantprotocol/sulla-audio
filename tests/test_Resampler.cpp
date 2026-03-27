#include <gtest/gtest.h>
#include <sulla/Resampler.h>
#include <cmath>

using namespace sulla;

TEST(Resampler, SameRate_NoOp) {
    float input[] = {1.0f, 2.0f, 3.0f, 4.0f};
    auto output = Resampler::resample(input, 4, 1, 48000, 48000);

    EXPECT_EQ(output.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(output[i], input[i]);
    }
}

TEST(Resampler, Downsample_48kTo16k) {
    // 48 frames at 48kHz = 1ms, should become 16 frames at 16kHz
    const uint32_t srcFrames = 480;
    std::vector<float> input(srcFrames);
    for (uint32_t i = 0; i < srcFrames; ++i) {
        input[i] = std::sin(2.0 * M_PI * 1000.0 * i / 48000.0); // 1kHz sine
    }

    auto output = Resampler::resample(input.data(), srcFrames, 1, 48000, 16000);

    uint32_t expected = Resampler::outputFrameCount(srcFrames, 48000, 16000);
    EXPECT_EQ(output.size(), expected);
    EXPECT_EQ(expected, 160u);
}

TEST(Resampler, Upsample_16kTo48k) {
    const uint32_t srcFrames = 160;
    std::vector<float> input(srcFrames, 0.5f);

    auto output = Resampler::resample(input.data(), srcFrames, 1, 16000, 48000);

    uint32_t expected = Resampler::outputFrameCount(srcFrames, 16000, 48000);
    EXPECT_EQ(output.size(), expected);
    EXPECT_EQ(expected, 480u);

    // All values should be close to 0.5 (constant input)
    for (float v : output) {
        EXPECT_NEAR(v, 0.5f, 0.01f);
    }
}

TEST(Resampler, Stereo) {
    // 2 frames of stereo: L=1.0 R=0.0, L=0.0 R=1.0
    float input[] = {1.0f, 0.0f, 0.0f, 1.0f};
    auto output = Resampler::resample(input, 2, 2, 48000, 48000);

    EXPECT_EQ(output.size(), 4u);
    EXPECT_FLOAT_EQ(output[0], 1.0f); // L
    EXPECT_FLOAT_EQ(output[1], 0.0f); // R
}

TEST(Resampler, EmptyInput) {
    auto output = Resampler::resample(nullptr, 0, 1, 48000, 16000);
    EXPECT_TRUE(output.empty());
}

TEST(Resampler, OutputFrameCount) {
    EXPECT_EQ(Resampler::outputFrameCount(48000, 48000, 16000), 16000u);
    EXPECT_EQ(Resampler::outputFrameCount(16000, 16000, 48000), 48000u);
    EXPECT_EQ(Resampler::outputFrameCount(1000, 48000, 48000), 1000u);
    EXPECT_EQ(Resampler::outputFrameCount(0, 48000, 16000), 0u);
}

TEST(Resampler, PreservesSignalShape) {
    // Generate a 100Hz sine at 48kHz, downsample to 16kHz, check it's still sinusoidal
    const uint32_t srcFrames = 4800; // 100ms
    std::vector<float> input(srcFrames);
    for (uint32_t i = 0; i < srcFrames; ++i) {
        input[i] = std::sin(2.0 * M_PI * 100.0 * i / 48000.0);
    }

    auto output = Resampler::resample(input.data(), srcFrames, 1, 48000, 16000);
    EXPECT_EQ(output.size(), 1600u);

    // Check that zero crossings are approximately preserved
    int zeroCrossings = 0;
    for (size_t i = 1; i < output.size(); ++i) {
        if ((output[i] >= 0) != (output[i-1] >= 0)) zeroCrossings++;
    }
    // 100Hz over 100ms = 10 cycles = 20 zero crossings
    EXPECT_NEAR(zeroCrossings, 20, 2);
}
