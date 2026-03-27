#include <gtest/gtest.h>
#include <sulla/AudioChunk.h>

using namespace sulla;

TEST(AudioChunk, SourceName) {
    AudioChunk mic;
    mic.source = AudioChunk::Source::Mic;
    EXPECT_EQ(mic.sourceName(), "mic");

    AudioChunk spk;
    spk.source = AudioChunk::Source::Speaker;
    EXPECT_EQ(spk.sourceName(), "speaker");
}

TEST(AudioChunk, ChannelNumber) {
    AudioChunk mic;
    mic.source = AudioChunk::Source::Mic;
    EXPECT_EQ(mic.channelNumber(), 0);

    AudioChunk spk;
    spk.source = AudioChunk::Source::Speaker;
    EXPECT_EQ(spk.channelNumber(), 1);
}

TEST(AudioChunk, WireFormatChannel0_NoPrefix) {
    AudioChunk chunk;
    chunk.source = AudioChunk::Source::Mic;
    chunk.audio = {0xAA, 0xBB, 0xCC, 0xDD};

    auto wire = chunk.toWireFormat();
    EXPECT_EQ(wire.size(), 4u);
    EXPECT_EQ(wire[0], 0xAA);
    EXPECT_EQ(wire[3], 0xDD);
}

TEST(AudioChunk, WireFormatChannel1_Tagged) {
    AudioChunk chunk;
    chunk.source = AudioChunk::Source::Speaker;
    chunk.audio = {0x11, 0x22, 0x33};

    auto wire = chunk.toWireFormat();
    EXPECT_EQ(wire.size(), 5u);  // 2 byte prefix + 3 bytes audio
    EXPECT_EQ(wire[0], 0x01);   // Magic byte
    EXPECT_EQ(wire[1], 0x01);   // Channel 1
    EXPECT_EQ(wire[2], 0x11);
    EXPECT_EQ(wire[4], 0x33);
}

TEST(AudioChunk, SizeBytes) {
    AudioChunk chunk;
    chunk.audio = {1, 2, 3, 4, 5};
    EXPECT_EQ(chunk.sizeBytes(), 5u);
}

TEST(AudioChunk, DurationMs) {
    AudioChunk chunk;
    chunk.sampleRate = 16000;
    chunk.channels = 1;
    chunk.bitDepth = 16;
    // 16000 samples/sec, 2 bytes/sample, 1 channel
    // 3200 bytes = 1600 frames = 100ms
    chunk.audio.resize(3200);

    EXPECT_NEAR(chunk.durationMs(), 100.0, 0.1);
}

TEST(AudioChunk, DurationMs_Stereo) {
    AudioChunk chunk;
    chunk.sampleRate = 48000;
    chunk.channels = 2;
    chunk.bitDepth = 16;
    // 48000 samples/sec, 2ch, 2 bytes/sample = 4 bytes/frame
    // 19200 bytes = 4800 frames = 100ms
    chunk.audio.resize(19200);

    EXPECT_NEAR(chunk.durationMs(), 100.0, 0.1);
}

TEST(AudioChunk, DurationMs_ZeroFormat) {
    AudioChunk chunk;
    chunk.sampleRate = 0;
    chunk.channels = 0;
    chunk.bitDepth = 0;
    chunk.audio.resize(100);
    EXPECT_EQ(chunk.durationMs(), 0.0);
}

TEST(AudioChunk, ToString) {
    AudioChunk chunk;
    chunk.source = AudioChunk::Source::Speaker;
    chunk.sequenceNum = 42;
    chunk.sampleRate = 16000;
    chunk.channels = 1;
    chunk.bitDepth = 16;
    chunk.audio.resize(3200);

    std::string s = chunk.toString();
    EXPECT_NE(s.find("speaker"), std::string::npos);
    EXPECT_NE(s.find("#42"), std::string::npos);
    EXPECT_NE(s.find("3200B"), std::string::npos);
    EXPECT_NE(s.find("16000Hz"), std::string::npos);
}

TEST(AudioChunk, WireFormatRoundTrip) {
    // Create a speaker chunk, encode to wire, verify structure
    AudioChunk original;
    original.source = AudioChunk::Source::Speaker;
    original.audio = {0xDE, 0xAD, 0xBE, 0xEF};

    auto wire = original.toWireFormat();

    // Parse it back using AudioChunkHeader (from GatewaySession.h)
    EXPECT_EQ(wire[0], 0x01);   // Magic
    EXPECT_EQ(wire[1], 0x01);   // Channel 1
    // Audio starts at offset 2
    EXPECT_EQ(wire[2], 0xDE);
    EXPECT_EQ(wire[5], 0xEF);
}
