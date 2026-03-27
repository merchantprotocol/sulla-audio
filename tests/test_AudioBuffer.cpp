#include <gtest/gtest.h>
#include <sulla/AudioBuffer.h>
#include <cmath>

using namespace sulla;

TEST(AudioBuffer, DefaultIsEmpty) {
    AudioBuffer buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.frameCount(), 0u);
    EXPECT_EQ(buf.sizeBytes(), 0u);
}

TEST(AudioBuffer, ConstructFromFloat) {
    float data[] = {0.5f, -0.5f, 0.25f, -0.25f};
    AudioFormat fmt{48000, 2, 32, true};
    AudioBuffer buf(data, 2, fmt); // 2 frames, stereo

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(buf.frameCount(), 2u);
    EXPECT_EQ(buf.sizeBytes(), 16u); // 2 frames * 2 ch * 4 bytes
    EXPECT_EQ(buf.format(), fmt);

    const float* out = buf.asFloat();
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FLOAT_EQ(out[1], -0.5f);
    EXPECT_FLOAT_EQ(out[2], 0.25f);
    EXPECT_FLOAT_EQ(out[3], -0.25f);
}

TEST(AudioBuffer, ConstructFromRawBytes) {
    int16_t data[] = {1000, -1000, 500, -500};
    AudioFormat fmt{16000, 1, 16, false};
    AudioBuffer buf(
        reinterpret_cast<const uint8_t*>(data),
        sizeof(data),
        fmt
    );

    EXPECT_EQ(buf.frameCount(), 4u); // 8 bytes / 2 bytes per frame
    EXPECT_EQ(buf.sizeBytes(), 8u);

    const int16_t* out = buf.asInt16();
    EXPECT_EQ(out[0], 1000);
    EXPECT_EQ(out[3], -500);
}

TEST(AudioBuffer, DurationMs) {
    float data[48000]; // 1 second of mono float
    std::fill_n(data, 48000, 0.0f);
    AudioFormat fmt{48000, 1, 32, true};
    AudioBuffer buf(data, 48000, fmt);

    EXPECT_DOUBLE_EQ(buf.durationMs(), 1000.0);
}

TEST(AudioBuffer, Allocate) {
    AudioBuffer buf;
    AudioFormat fmt = AudioFormat::telephony();
    buf.allocate(16000, fmt); // 1 second

    EXPECT_EQ(buf.frameCount(), 16000u);
    EXPECT_EQ(buf.sizeBytes(), 32000u); // 16000 frames * 2 bytes
    EXPECT_EQ(buf.format(), fmt);
    EXPECT_FALSE(buf.empty());
}
