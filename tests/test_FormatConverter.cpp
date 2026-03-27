#include <gtest/gtest.h>
#include <sulla/FormatConverter.h>
#include <cmath>

using namespace sulla;

TEST(FormatConverter, FloatToInt16_FullRange) {
    float input[] = {1.0f, -1.0f, 0.0f, 0.5f};
    auto output = FormatConverter::floatToInt16(input, 4);

    EXPECT_EQ(output[0], 32767);
    EXPECT_EQ(output[1], -32767);
    EXPECT_EQ(output[2], 0);
    EXPECT_EQ(output[3], 16383); // 0.5 * 32767 ≈ 16383
}

TEST(FormatConverter, FloatToInt16_Clamping) {
    float input[] = {2.0f, -2.0f}; // Out of range
    auto output = FormatConverter::floatToInt16(input, 2);

    EXPECT_EQ(output[0], 32767);  // Clamped to max
    EXPECT_EQ(output[1], -32767); // Clamped to min
}

TEST(FormatConverter, Int16ToFloat_FullRange) {
    int16_t input[] = {32767, -32768, 0, 16384};
    auto output = FormatConverter::int16ToFloat(input, 4);

    EXPECT_NEAR(output[0], 1.0f, 0.001f);
    EXPECT_NEAR(output[1], -1.0f, 0.001f);
    EXPECT_FLOAT_EQ(output[2], 0.0f);
    EXPECT_NEAR(output[3], 0.5f, 0.001f);
}

TEST(FormatConverter, RoundTrip_FloatInt16Float) {
    float original[] = {0.25f, -0.75f, 0.0f, 0.1f};
    auto int16 = FormatConverter::floatToInt16(original, 4);
    auto back = FormatConverter::int16ToFloat(int16.data(), 4);

    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(back[i], original[i], 0.001f);
    }
}

TEST(FormatConverter, StereoToMono_Float) {
    float stereo[] = {0.8f, 0.2f, -0.4f, -0.6f}; // L, R, L, R
    auto mono = FormatConverter::stereoToMono(stereo, 2);

    EXPECT_EQ(mono.size(), 2u);
    EXPECT_FLOAT_EQ(mono[0], 0.5f);  // (0.8 + 0.2) / 2
    EXPECT_FLOAT_EQ(mono[1], -0.5f); // (-0.4 + -0.6) / 2
}

TEST(FormatConverter, StereoToMono_Int16) {
    int16_t stereo[] = {1000, 2000, -1000, -2000};
    auto mono = FormatConverter::stereoToMono(stereo, 2);

    EXPECT_EQ(mono.size(), 2u);
    EXPECT_EQ(mono[0], 1500);  // (1000 + 2000) / 2
    EXPECT_EQ(mono[1], -1500);
}

TEST(FormatConverter, MonoToStereo) {
    float mono[] = {0.5f, -0.3f};
    auto stereo = FormatConverter::monoToStereo(mono, 2);

    EXPECT_EQ(stereo.size(), 4u);
    EXPECT_FLOAT_EQ(stereo[0], 0.5f);
    EXPECT_FLOAT_EQ(stereo[1], 0.5f);
    EXPECT_FLOAT_EQ(stereo[2], -0.3f);
    EXPECT_FLOAT_EQ(stereo[3], -0.3f);
}

TEST(FormatConverter, ConvertBuffer_StereoFloatToMonoInt16) {
    float data[] = {0.5f, 0.5f, -0.5f, -0.5f}; // 2 frames stereo
    AudioFormat srcFmt{48000, 2, 32, true};
    AudioBuffer src(data, 2, srcFmt);

    AudioFormat dstFmt{48000, 1, 16, false};
    AudioBuffer dst = FormatConverter::convert(src, dstFmt);

    EXPECT_EQ(dst.frameCount(), 2u);
    const int16_t* out = dst.asInt16();
    EXPECT_NEAR(out[0], 16383, 2);  // 0.5 * 32767
    EXPECT_NEAR(out[1], -16383, 2);
}

TEST(FormatConverter, EmptyInput) {
    auto result = FormatConverter::floatToInt16(nullptr, 0);
    EXPECT_TRUE(result.empty());

    auto result2 = FormatConverter::stereoToMono<float>(nullptr, 0);
    EXPECT_TRUE(result2.empty());
}
