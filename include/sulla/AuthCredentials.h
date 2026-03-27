#pragma once

#include <string>
#include <cstdint>

namespace sulla {

/**
 * AuthCredentials — secure credential storage model.
 *
 * Pure data. Holds login credentials and the resulting JWT.
 * NEVER log or serialize the password or raw token — use redacted() for display.
 *
 * Flow:
 *   1. User provides email + password (via config UI or CLI)
 *   2. IAuthClient sends POST /auth/login → receives JWT
 *   3. JWT is stored here and used for all subsequent requests
 *   4. User ID is extracted from the JWT payload
 */
struct AuthCredentials {
    // Input (from user)
    std::string email;
    std::string password;    // NEVER log this

    // Output (from server)
    std::string token;       // JWT — NEVER log raw, use redacted()
    std::string userId;      // Extracted from JWT claims
    std::string companyId;
    int64_t     expiresAt = 0;  // Unix timestamp

    /** Whether we have valid login credentials to attempt auth. */
    bool hasCredentials() const {
        return !email.empty() && !password.empty();
    }

    /** Whether we have a token (may or may not be expired). */
    bool hasToken() const {
        return !token.empty();
    }

    /** Whether the token appears expired (client-side check). */
    bool isExpired(int64_t nowUnix) const {
        return expiresAt > 0 && nowUnix >= expiresAt;
    }

    /** Whether we have everything needed to make authenticated requests. */
    bool isAuthenticated() const {
        return !token.empty() && !userId.empty();
    }

    /** Safe display string — redacts sensitive fields. */
    std::string redacted() const {
        return "email=" + email
             + " userId=" + userId
             + " token=" + (token.empty() ? "(none)" : "***")
             + " expires=" + std::to_string(expiresAt);
    }

    /** Clear all credential data. */
    void clear() {
        email.clear();
        password.clear();
        token.clear();
        userId.clear();
        companyId.clear();
        expiresAt = 0;
    }
};

/**
 * AuthResult — response from an authentication attempt.
 */
struct AuthResult {
    bool        success = false;
    std::string error;
    AuthCredentials credentials;

    static AuthResult ok(AuthCredentials creds) {
        return {true, "", std::move(creds)};
    }

    static AuthResult fail(const std::string& reason) {
        return {false, reason, {}};
    }
};

} // namespace sulla
