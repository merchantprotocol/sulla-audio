#include <gtest/gtest.h>
#include <sulla/Logger.h>

using namespace sulla;

TEST(LoggerRedact, BearerToken) {
    std::string input = "Authorization: Bearer eyJhbGciOiJSUzI1NiJ9.payload.sig";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("eyJhbGciOiJSUzI1NiJ9"), std::string::npos);
    EXPECT_NE(result.find("Bearer ***"), std::string::npos);
}

TEST(LoggerRedact, TokenQueryParam) {
    std::string input = "ws://gateway.com/audio/123?token=abc123def456";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("abc123def456"), std::string::npos);
    EXPECT_NE(result.find("token=***"), std::string::npos);
}

TEST(LoggerRedact, ApiKey) {
    std::string input = "api_key=mpk_secret_key_123";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("mpk_secret_key_123"), std::string::npos);
    EXPECT_NE(result.find("api_key=***"), std::string::npos);
}

TEST(LoggerRedact, ApiKeyColon) {
    std::string input = "api_key: my_secret_value";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("my_secret_value"), std::string::npos);
}

TEST(LoggerRedact, Password) {
    std::string input = R"({"email":"user@test.com","password":"hunter2"})";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("hunter2"), std::string::npos);
    EXPECT_NE(result.find("\"password\":\"***\""), std::string::npos);
}

TEST(LoggerRedact, MpkPrefixedKey) {
    std::string input = "Using key mpk_live_abc123def456";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("mpk_live_abc123def456"), std::string::npos);
    EXPECT_NE(result.find("mpk_***"), std::string::npos);
}

TEST(LoggerRedact, JwtToken) {
    std::string input = "token is eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ.dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("eyJhbGciOiJIUzI1NiJ9"), std::string::npos);
    EXPECT_NE(result.find("***jwt***"), std::string::npos);
}

TEST(LoggerRedact, NoFalsePositives) {
    std::string input = "Connected to gateway.example.com on port 8080";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result, input); // Nothing to redact
}

TEST(LoggerRedact, MultipleSecrets) {
    std::string input = "Bearer abc123.def456.ghi789 and api_key=secret123 and mpk_test_key";
    std::string result = Logger::redact(input);
    EXPECT_EQ(result.find("abc123"), std::string::npos);
    EXPECT_EQ(result.find("secret123"), std::string::npos);
    EXPECT_EQ(result.find("mpk_test_key"), std::string::npos);
}
