#pragma once

#include "base/error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace popo::server
{

inline constexpr const char kServerVersion[] = "5.7.42-popodb";
inline constexpr const char kCachingSha2[] = "caching_sha2_password";
inline constexpr const char kNativePassword[] = "mysql_native_password";
inline constexpr const char kSha256Password[] = "sha256_password";
inline constexpr const char kClearPassword[] = "mysql_clear_password";

struct AuthConfig
{
    std::string password;
    std::string plugin = kCachingSha2;
    // Ask caching_sha2 clients for the RSA exchange even when the fast hash matches.
    bool require_full_auth = false;
    std::string bind_address = "0.0.0.0";
    // Empty datadir keeps accounts in memory. main() sets this from the config file.
    std::string datadir;
    std::string database = "popo";
};

// Server RSA key used by caching_sha2_password and sha256_password.
class AuthKeys
{
public:
    static popo::base::Result<std::unique_ptr<AuthKeys>> create();

    ~AuthKeys();
    AuthKeys(const AuthKeys &) = delete;
    AuthKeys & operator=(const AuthKeys &) = delete;

    const std::string & public_pem() const { return public_pem_; }

    // Undo the client's RSA-OAEP(SHA-1) encryption and scramble XOR.
    popo::base::Result<std::string>
    decrypt_password(const uint8_t * cipher, size_t len, const uint8_t * scramble, size_t scramble_len) const;

private:
    struct Impl;
    AuthKeys(std::unique_ptr<Impl> impl, std::string public_pem);

    std::unique_ptr<Impl> impl_;
    std::string public_pem_;
};

popo::base::Result<std::vector<uint8_t>> make_scramble();
popo::base::Result<std::vector<uint8_t>>
native_password_token(const std::string & password, const std::vector<uint8_t> & scramble);
popo::base::Result<std::vector<uint8_t>>
caching_sha2_token(const std::string & password, const std::vector<uint8_t> & scramble);
bool same_bytes(const std::vector<uint8_t> & expected, const std::vector<uint8_t> & actual);

} // namespace popo::server
