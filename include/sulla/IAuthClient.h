#pragma once

#include <memory>
#include <string>
#include "AuthCredentials.h"

namespace sulla {

/**
 * IAuthClient — interface for authenticating against the backend API.
 *
 * No business decisions. Just sends credentials, gets token back.
 *
 * Flow:
 *   1. POST {backendUrl}/auth/login  { email, password }
 *   2. Response: { token, user: { id, email, company_id, company_name } }
 *   3. Extract user_id from JWT claims (or response body)
 *   4. Token is used in Authorization header for all subsequent requests
 *
 * Security:
 *   - Password is sent over TLS only (HTTPS)
 *   - Token goes in Authorization header, never in URL query params
 *   - Neither password nor token is logged (Logger::redact handles this)
 */
class IAuthClient {
public:
    virtual ~IAuthClient() = default;

    /**
     * Authenticate with email and password.
     * Returns AuthResult with token and user info on success.
     */
    virtual AuthResult login(const std::string& backendUrl,
                              const std::string& email,
                              const std::string& password) = 0;

    /**
     * Refresh an existing token before it expires.
     * Returns new AuthResult with fresh token.
     */
    virtual AuthResult refreshToken(const std::string& backendUrl,
                                     const std::string& currentToken) = 0;

    /**
     * Extract the user_id from a JWT token (decode payload without verification).
     * This is a utility — doesn't make network calls.
     */
    virtual std::string extractUserId(const std::string& token) = 0;

    /** Factory. */
    static std::unique_ptr<IAuthClient> create();
};

} // namespace sulla
