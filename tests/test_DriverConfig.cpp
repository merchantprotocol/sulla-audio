#include <gtest/gtest.h>
#include <sulla/DriverConfig.h>

using namespace sulla;

TEST(DriverConfig, DefaultIsLocalMode) {
    DriverConfig cfg;
    EXPECT_FALSE(cfg.isGatewayMode());
    EXPECT_TRUE(cfg.isLocalMode());
    EXPECT_TRUE(cfg.gatewayUrl.empty());
    EXPECT_TRUE(cfg.backendUrl.empty());
    EXPECT_TRUE(cfg.email.empty());
    EXPECT_EQ(cfg.chunkIntervalMs, 200u);
    EXPECT_EQ(cfg.targetSampleRate, 16000u);
    EXPECT_EQ(cfg.targetBitDepth, 16u);
    EXPECT_EQ(cfg.targetChannels, 1u);
    EXPECT_TRUE(cfg.captureMic);
    EXPECT_TRUE(cfg.captureSpeaker);
    EXPECT_TRUE(cfg.autoStart);
}

TEST(DriverConfig, HasGatewayConfig) {
    DriverConfig cfg;
    EXPECT_FALSE(cfg.hasGatewayConfig());

    cfg.backendUrl = "https://api.example.com";
    EXPECT_FALSE(cfg.hasGatewayConfig()); // Missing email

    cfg.email = "user@example.com";
    EXPECT_TRUE(cfg.hasGatewayConfig());
}

TEST(DriverConfig, HasLocalConfig) {
    DriverConfig cfg;
    EXPECT_FALSE(cfg.hasLocalConfig());

    cfg.localSocketPath = "/tmp/audio-driver.sock";
    EXPECT_TRUE(cfg.hasLocalConfig());

    cfg.localSocketPath.clear();
    cfg.localPort = 9999;
    EXPECT_TRUE(cfg.hasLocalConfig());
}

TEST(DriverConfig, SerializeDeserializeGateway) {
    DriverConfig original;
    original.mode = DriverConfig::Mode::Gateway;
    original.backendUrl = "https://api.example.com";
    original.email = "user@test.com";
    original.gatewayUrl = "wss://gateway.example.com";
    original.preferredDevice = "device-abc";
    original.chunkIntervalMs = 500;
    original.targetSampleRate = 44100;
    original.targetBitDepth = 24;
    original.targetChannels = 2;
    original.captureMic = false;
    original.captureSpeaker = true;
    original.autoStart = false;
    original.logLevel = "debug";
    original.logAudioDiagnostics = false;

    std::string serialized = original.serialize();
    DriverConfig restored = DriverConfig::deserialize(serialized);

    EXPECT_TRUE(restored.isGatewayMode());
    EXPECT_EQ(restored.backendUrl, original.backendUrl);
    EXPECT_EQ(restored.email, original.email);
    EXPECT_EQ(restored.gatewayUrl, original.gatewayUrl);
    EXPECT_EQ(restored.preferredDevice, original.preferredDevice);
    EXPECT_EQ(restored.chunkIntervalMs, original.chunkIntervalMs);
    EXPECT_EQ(restored.targetSampleRate, original.targetSampleRate);
    EXPECT_EQ(restored.targetBitDepth, original.targetBitDepth);
    EXPECT_EQ(restored.targetChannels, original.targetChannels);
    EXPECT_EQ(restored.captureMic, original.captureMic);
    EXPECT_EQ(restored.captureSpeaker, original.captureSpeaker);
    EXPECT_EQ(restored.autoStart, original.autoStart);
    EXPECT_EQ(restored.logLevel, original.logLevel);
    EXPECT_EQ(restored.logAudioDiagnostics, original.logAudioDiagnostics);
}

TEST(DriverConfig, SerializeDeserializeLocal) {
    DriverConfig original;
    original.mode = DriverConfig::Mode::Local;
    original.localSocketPath = "/tmp/audio-driver.sock";
    original.localPort = 8080;

    std::string serialized = original.serialize();
    DriverConfig restored = DriverConfig::deserialize(serialized);

    EXPECT_TRUE(restored.isLocalMode());
    EXPECT_EQ(restored.localSocketPath, original.localSocketPath);
    EXPECT_EQ(restored.localPort, original.localPort);
}

TEST(DriverConfig, DeserializeIgnoresCommentsAndHeaders) {
    std::string content =
        "# This is a comment\n"
        "[mode]\n"
        "mode=local\n"
        "\n"
        "[local]\n"
        "socket_path=/tmp/test.sock\n"
        "port=1234\n"
        "\n"
        "[audio]\n"
        "chunk_interval_ms=100\n";

    auto cfg = DriverConfig::deserialize(content);
    EXPECT_TRUE(cfg.isLocalMode());
    EXPECT_EQ(cfg.localSocketPath, "/tmp/test.sock");
    EXPECT_EQ(cfg.localPort, 1234u);
    EXPECT_EQ(cfg.chunkIntervalMs, 100u);
}

TEST(DriverConfig, DeserializeEmptyString) {
    auto cfg = DriverConfig::deserialize("");
    EXPECT_TRUE(cfg.isLocalMode()); // Default
    EXPECT_FALSE(cfg.hasGatewayConfig());
}

TEST(DriverConfig, AuthEndpoint) {
    DriverConfig cfg;
    cfg.backendUrl = "https://api.example.com";
    EXPECT_EQ(cfg.authEndpoint(), "https://api.example.com/auth/login");
}

TEST(DriverConfig, SessionEndpoint_Wss) {
    DriverConfig cfg;
    cfg.gatewayUrl = "wss://gateway.example.com";
    EXPECT_EQ(cfg.sessionEndpoint(), "https://gateway.example.com/api/sessions");
}

TEST(DriverConfig, SessionEndpoint_Ws) {
    DriverConfig cfg;
    cfg.gatewayUrl = "ws://localhost:8081";
    EXPECT_EQ(cfg.sessionEndpoint(), "http://localhost:8081/api/sessions");
}

TEST(DriverConfig, AudioEndpointBase) {
    DriverConfig cfg;
    cfg.gatewayUrl = "wss://gateway.example.com";
    EXPECT_EQ(cfg.audioEndpointBase(), "wss://gateway.example.com/audio/");
}

TEST(DriverConfig, DefaultLocalSocket) {
    std::string socket = DriverConfig::defaultLocalSocket();
    EXPECT_FALSE(socket.empty());
}

TEST(DriverConfig, ConfigDirIsNonEmpty) {
    std::string dir = DriverConfig::configDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_NE(dir, "/");
}

TEST(DriverConfig, ConfigFilePathIncludesFilename) {
    std::string path = DriverConfig::configFilePath();
    EXPECT_NE(path.find("config.ini"), std::string::npos);
}

TEST(DriverConfig, SecurityPasswordNeverSerialized) {
    DriverConfig cfg;
    cfg.email = "user@test.com";
    cfg.backendUrl = "https://api.example.com";
    std::string serialized = cfg.serialize();

    // password should never appear in serialized output
    EXPECT_EQ(serialized.find("password="), std::string::npos);
    // token should never appear
    EXPECT_EQ(serialized.find("token="), std::string::npos);
}
