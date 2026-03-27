#include <gtest/gtest.h>
#include <sulla/AuthCredentials.h>

using namespace sulla;

TEST(AuthCredentials, DefaultIsEmpty) {
    AuthCredentials creds;
    EXPECT_FALSE(creds.hasCredentials());
    EXPECT_FALSE(creds.hasToken());
    EXPECT_FALSE(creds.isAuthenticated());
}

TEST(AuthCredentials, HasCredentials) {
    AuthCredentials creds;
    creds.email = "user@test.com";
    EXPECT_FALSE(creds.hasCredentials());

    creds.password = "secret";
    EXPECT_TRUE(creds.hasCredentials());
}

TEST(AuthCredentials, HasToken) {
    AuthCredentials creds;
    EXPECT_FALSE(creds.hasToken());

    creds.token = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ.abc";
    EXPECT_TRUE(creds.hasToken());
}

TEST(AuthCredentials, IsAuthenticated) {
    AuthCredentials creds;
    creds.token = "some-token";
    EXPECT_FALSE(creds.isAuthenticated()); // Missing userId

    creds.userId = "user-123";
    EXPECT_TRUE(creds.isAuthenticated());
}

TEST(AuthCredentials, IsExpired) {
    AuthCredentials creds;
    creds.expiresAt = 1000;

    EXPECT_FALSE(creds.isExpired(500));  // Before expiry
    EXPECT_TRUE(creds.isExpired(1000));  // At expiry
    EXPECT_TRUE(creds.isExpired(1500));  // After expiry

    // Zero expiresAt means never expires
    creds.expiresAt = 0;
    EXPECT_FALSE(creds.isExpired(99999));
}

TEST(AuthCredentials, RedactedHidesToken) {
    AuthCredentials creds;
    creds.email = "user@test.com";
    creds.userId = "u42";
    creds.token = "super-secret-jwt";
    creds.expiresAt = 12345;

    std::string display = creds.redacted();
    EXPECT_NE(display.find("user@test.com"), std::string::npos);
    EXPECT_NE(display.find("u42"), std::string::npos);
    EXPECT_NE(display.find("***"), std::string::npos);
    EXPECT_EQ(display.find("super-secret-jwt"), std::string::npos);
}

TEST(AuthCredentials, RedactedShowsNoneWhenNoToken) {
    AuthCredentials creds;
    creds.email = "test@x.com";
    std::string display = creds.redacted();
    EXPECT_NE(display.find("(none)"), std::string::npos);
}

TEST(AuthCredentials, Clear) {
    AuthCredentials creds;
    creds.email = "user@test.com";
    creds.password = "secret";
    creds.token = "jwt";
    creds.userId = "u1";
    creds.companyId = "c1";
    creds.expiresAt = 9999;

    creds.clear();

    EXPECT_TRUE(creds.email.empty());
    EXPECT_TRUE(creds.password.empty());
    EXPECT_TRUE(creds.token.empty());
    EXPECT_TRUE(creds.userId.empty());
    EXPECT_TRUE(creds.companyId.empty());
    EXPECT_EQ(creds.expiresAt, 0);
}

TEST(AuthResult, OkResult) {
    AuthCredentials creds;
    creds.email = "user@test.com";
    creds.token = "jwt";
    creds.userId = "u1";

    auto result = AuthResult::ok(creds);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.credentials.email, "user@test.com");
    EXPECT_TRUE(result.credentials.isAuthenticated());
}

TEST(AuthResult, FailResult) {
    auto result = AuthResult::fail("Invalid credentials");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "Invalid credentials");
    EXPECT_FALSE(result.credentials.isAuthenticated());
}
