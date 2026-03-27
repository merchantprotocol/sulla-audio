#include <gtest/gtest.h>
#include <sulla/GatewaySession.h>

using namespace sulla;

TEST(GatewaySession, DefaultIsInvalid) {
    GatewaySession s;
    EXPECT_FALSE(s.isValid());
}

TEST(GatewaySession, ValidWhenPopulated) {
    GatewaySession s;
    s.sessionId = "sess-123";
    s.audioUrl = "ws://gateway/audio/sess-123";
    EXPECT_TRUE(s.isValid());
}

TEST(GatewaySession, InvalidWithoutAudioUrl) {
    GatewaySession s;
    s.sessionId = "sess-123";
    EXPECT_FALSE(s.isValid());
}

// ─── ChannelMap ──────────────────────────────────────────────

TEST(ChannelMap, SingleChannel) {
    auto m = ChannelMap::singleChannel();
    EXPECT_EQ(m.channels.size(), 1u);
    EXPECT_FALSE(m.isMultiChannel());
    EXPECT_EQ(m.channels[0].label, "User");
    EXPECT_EQ(m.channels[0].source, "mic");
}

TEST(ChannelMap, DualChannel) {
    auto m = ChannelMap::dualChannel();
    EXPECT_EQ(m.channels.size(), 2u);
    EXPECT_TRUE(m.isMultiChannel());
    EXPECT_EQ(m.channels[0].label, "User");
    EXPECT_EQ(m.channels[0].source, "mic");
    EXPECT_EQ(m.channels[1].label, "Caller");
    EXPECT_EQ(m.channels[1].source, "system_audio");
}

// ─── AudioChunkHeader ────────────────────────────────────────

TEST(AudioChunkHeader, Channel0_NoPrefix) {
    uint8_t audio[] = {0xAA, 0xBB, 0xCC};
    auto tagged = AudioChunkHeader::tag(audio, 3, 0);

    EXPECT_EQ(tagged.size(), 3u);
    EXPECT_EQ(tagged[0], 0xAA);
    EXPECT_EQ(tagged[1], 0xBB);
    EXPECT_EQ(tagged[2], 0xCC);
}

TEST(AudioChunkHeader, Channel1_HasPrefix) {
    uint8_t audio[] = {0xAA, 0xBB};
    auto tagged = AudioChunkHeader::tag(audio, 2, 1);

    EXPECT_EQ(tagged.size(), 4u);
    EXPECT_EQ(tagged[0], 0x01); // Magic
    EXPECT_EQ(tagged[1], 0x01); // Channel 1
    EXPECT_EQ(tagged[2], 0xAA);
    EXPECT_EQ(tagged[3], 0xBB);
}

TEST(AudioChunkHeader, ParseChannel0_Untagged) {
    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    auto result = AudioChunkHeader::parse(data, 3);

    EXPECT_EQ(result.channel, 0);
    EXPECT_EQ(result.audioOffset, 0u);
}

TEST(AudioChunkHeader, ParseChannel1_Tagged) {
    uint8_t data[] = {0x01, 0x01, 0xAA, 0xBB};
    auto result = AudioChunkHeader::parse(data, 4);

    EXPECT_EQ(result.channel, 1);
    EXPECT_EQ(result.audioOffset, 2u);
}

TEST(AudioChunkHeader, ParseChannel255) {
    uint8_t data[] = {0x01, 0xFF, 0xDD};
    auto result = AudioChunkHeader::parse(data, 3);

    EXPECT_EQ(result.channel, 255);
    EXPECT_EQ(result.audioOffset, 2u);
}

TEST(AudioChunkHeader, RoundTrip) {
    uint8_t audio[] = {0x11, 0x22, 0x33, 0x44};

    for (uint8_t ch = 0; ch < 5; ++ch) {
        auto tagged = AudioChunkHeader::tag(audio, 4, ch);
        auto parsed = AudioChunkHeader::parse(tagged.data(), tagged.size());

        EXPECT_EQ(parsed.channel, ch);

        // Verify audio bytes match
        const uint8_t* audioStart = tagged.data() + parsed.audioOffset;
        size_t audioLen = tagged.size() - parsed.audioOffset;
        EXPECT_EQ(audioLen, 4u);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(audioStart[i], audio[i]);
        }
    }
}
