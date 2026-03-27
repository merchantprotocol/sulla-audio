#include <gtest/gtest.h>
#include <sulla/AudioFormat.h>

using namespace sulla;

TEST(AudioFormat, BytesPerSample) {
    AudioFormat f1{48000, 2, 32, true};
    EXPECT_EQ(f1.bytesPerSample(), 4u);
    AudioFormat f2{16000, 1, 16, false};
    EXPECT_EQ(f2.bytesPerSample(), 2u);
    AudioFormat f3{44100, 2, 24, false};
    EXPECT_EQ(f3.bytesPerSample(), 3u);
}

TEST(AudioFormat, BytesPerFrame) {
    AudioFormat f1{48000, 2, 32, true};
    EXPECT_EQ(f1.bytesPerFrame(), 8u);
    AudioFormat f2{16000, 1, 16, false};
    EXPECT_EQ(f2.bytesPerFrame(), 2u);
    AudioFormat f3{48000, 6, 32, true};
    EXPECT_EQ(f3.bytesPerFrame(), 24u);
}

TEST(AudioFormat, BytesPerSecond) {
    auto fmt = AudioFormat::telephony();
    EXPECT_EQ(fmt.bytesPerSecond(), 32000u);

    auto wasapi = AudioFormat::wasapi();
    EXPECT_EQ(wasapi.bytesPerSecond(), 384000u);
}

TEST(AudioFormat, FramesToMs) {
    AudioFormat fmt{48000, 2, 32, true};
    EXPECT_DOUBLE_EQ(fmt.framesToMs(48000), 1000.0);
    EXPECT_DOUBLE_EQ(fmt.framesToMs(24000), 500.0);
    EXPECT_DOUBLE_EQ(fmt.framesToMs(0), 0.0);
}

TEST(AudioFormat, MsToFrames) {
    AudioFormat fmt{48000, 2, 32, true};
    EXPECT_EQ(fmt.msToFrames(1000.0), 48000u);
    EXPECT_EQ(fmt.msToFrames(250.0), 12000u);
    EXPECT_EQ(fmt.msToFrames(0.0), 0u);
}

TEST(AudioFormat, Equality) {
    auto a = AudioFormat::wasapi();
    auto b = AudioFormat::wasapi();
    EXPECT_EQ(a, b);

    auto c = AudioFormat::telephony();
    EXPECT_NE(a, c);
}

TEST(AudioFormat, Presets) {
    auto cd = AudioFormat::cd();
    EXPECT_EQ(cd.sampleRate, 44100u);
    EXPECT_EQ(cd.channels, 2u);
    EXPECT_EQ(cd.bitDepth, 16u);
    EXPECT_FALSE(cd.isFloat);

    auto tel = AudioFormat::telephony();
    EXPECT_EQ(tel.sampleRate, 16000u);
    EXPECT_EQ(tel.channels, 1u);

    auto wasapi = AudioFormat::wasapi();
    EXPECT_EQ(wasapi.sampleRate, 48000u);
    EXPECT_TRUE(wasapi.isFloat);
}

TEST(AudioFormat, ToString) {
    auto fmt = AudioFormat::wasapi();
    EXPECT_EQ(fmt.toString(), "48000Hz, 32-bit float, 2ch");

    auto tel = AudioFormat::telephony();
    EXPECT_EQ(tel.toString(), "16000Hz, 16-bit int, 1ch");
}

TEST(AudioFormat, ZeroSampleRateHandled) {
    AudioFormat fmt{0, 2, 32, true};
    EXPECT_DOUBLE_EQ(fmt.framesToMs(100), 0.0);
    EXPECT_EQ(fmt.bytesPerSecond(), 0u);
}
