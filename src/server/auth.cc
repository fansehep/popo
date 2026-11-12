#include "server/auth.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <new>
#include <utility>

namespace popo::server
{

namespace
{

struct EvpMdCtx
{
    EVP_MD_CTX * ctx = EVP_MD_CTX_new();
    ~EvpMdCtx() { EVP_MD_CTX_free(ctx); }
};

struct EvpPkeyCtx
{
    EVP_PKEY_CTX * ctx = nullptr;
    ~EvpPkeyCtx() { EVP_PKEY_CTX_free(ctx); }
};

struct Bio
{
    BIO * bio = nullptr;
    ~Bio() { BIO_free(bio); }
};

popo::base::Result<std::vector<uint8_t>>
digest(const EVP_MD * md, const uint8_t * a, size_t a_len, const uint8_t * b = nullptr, size_t b_len = 0)
{
    EvpMdCtx ctx;
    if (!ctx.ctx || EVP_DigestInit_ex(ctx.ctx, md, nullptr) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "digest init failed");
    }
    if (EVP_DigestUpdate(ctx.ctx, a, a_len) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "digest update failed");
    }
    if (b != nullptr && b_len > 0 && EVP_DigestUpdate(ctx.ctx, b, b_len) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "digest update failed");
    }
    const int digest_size = EVP_MD_get_size(md);
    if (digest_size <= 0)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "invalid digest size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(digest_size));
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.ctx, out.data(), &len) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "digest final failed");
    }
    out.resize(len);
    return out;
}

void xor_bytes(std::vector<uint8_t> & left, const std::vector<uint8_t> & right)
{
    for (size_t i = 0; i < left.size() && i < right.size(); ++i)
    {
        left[i] = static_cast<uint8_t>(left[i] ^ right[i]);
    }
}

} // namespace

struct AuthKeys::Impl
{
    EVP_PKEY * key = nullptr;
    ~Impl() { EVP_PKEY_free(key); }
};

AuthKeys::AuthKeys(std::unique_ptr<Impl> impl, std::string public_pem)
    : impl_(std::move(impl)), public_pem_(std::move(public_pem))
{
}

popo::base::Result<std::unique_ptr<AuthKeys>> AuthKeys::create()
{
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
    if (!impl)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA key allocation failed");
    }

    EvpPkeyCtx ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (ctx.ctx == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA context failed");
    }
    if (EVP_PKEY_keygen_init(ctx.ctx) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.ctx, 2048) != 1
        || EVP_PKEY_keygen(ctx.ctx, &impl->key) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA key generation failed");
    }

    Bio bio{BIO_new(BIO_s_mem())};
    if (bio.bio == nullptr || PEM_write_bio_PUBKEY(bio.bio, impl->key) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA public key export failed");
    }

    char * data = nullptr;
    const long len = BIO_get_mem_data(bio.bio, &data);
    if (len <= 0 || data == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA public key export was empty");
    }
    std::string public_pem(data, static_cast<size_t>(len));
    std::unique_ptr<AuthKeys> keys(new (std::nothrow) AuthKeys(std::move(impl), std::move(public_pem)));
    if (!keys)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA key holder allocation failed");
    }
    return keys;
}

AuthKeys::~AuthKeys() = default;

popo::base::Result<std::string>
AuthKeys::decrypt_password(const uint8_t * cipher, size_t len, const uint8_t * scramble, size_t scramble_len) const
{
    if (cipher == nullptr || len == 0 || scramble == nullptr || scramble_len == 0 || impl_->key == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "invalid encrypted password input");
    }
    EvpPkeyCtx ctx{EVP_PKEY_CTX_new(impl_->key, nullptr)};
    if (ctx.ctx == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA decrypt context failed");
    }
    bool ok = EVP_PKEY_decrypt_init(ctx.ctx) == 1 && EVP_PKEY_CTX_set_rsa_padding(ctx.ctx, RSA_PKCS1_OAEP_PADDING) == 1
        && EVP_PKEY_CTX_set_rsa_oaep_md(ctx.ctx, EVP_sha1()) == 1
        && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.ctx, EVP_sha1()) == 1;
    if (!ok)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "RSA decrypt init failed");
    }
    size_t out_len = 0;
    if (EVP_PKEY_decrypt(ctx.ctx, nullptr, &out_len, cipher, len) != 1)
    {
        return popo::base::make_error(popo::base::Errno::AuthDenied, "encrypted password is invalid");
    }
    std::vector<uint8_t> plain(out_len);
    if (EVP_PKEY_decrypt(ctx.ctx, plain.data(), &out_len, cipher, len) != 1)
    {
        return popo::base::make_error(popo::base::Errno::AuthDenied, "encrypted password is invalid");
    }
    plain.resize(out_len);

    std::string password;
    password.reserve(plain.size());
    for (size_t i = 0; i < plain.size(); ++i)
    {
        auto ch = static_cast<char>(plain[i] ^ scramble[i % scramble_len]);
        if (ch == '\0')
        {
            break;
        }
        password.push_back(ch);
    }
    return password;
}

popo::base::Result<std::vector<uint8_t>> make_scramble()
{
    std::vector<uint8_t> scramble(20);
    if (RAND_bytes(scramble.data(), static_cast<int>(scramble.size())) != 1)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "scramble generation failed");
    }
    for (auto & b : scramble)
    {
        if (b == 0)
        {
            b = 1;
        }
    }
    return scramble;
}

popo::base::Result<std::vector<uint8_t>>
native_password_token(const std::string & password, const std::vector<uint8_t> & scramble)
{
    auto stage1 = digest(EVP_sha1(), reinterpret_cast<const uint8_t *>(password.data()), password.size());
    if (!stage1)
        return std::unexpected(stage1.error());
    auto stage2 = digest(EVP_sha1(), stage1->data(), stage1->size());
    if (!stage2)
        return std::unexpected(stage2.error());
    auto mixed = digest(EVP_sha1(), scramble.data(), scramble.size(), stage2->data(), stage2->size());
    if (!mixed)
        return std::unexpected(mixed.error());
    xor_bytes(*stage1, *mixed);
    return std::move(*stage1);
}

popo::base::Result<std::vector<uint8_t>>
caching_sha2_token(const std::string & password, const std::vector<uint8_t> & scramble)
{
    auto hash1 = digest(EVP_sha256(), reinterpret_cast<const uint8_t *>(password.data()), password.size());
    if (!hash1)
        return std::unexpected(hash1.error());
    auto hash2 = digest(EVP_sha256(), hash1->data(), hash1->size());
    if (!hash2)
        return std::unexpected(hash2.error());
    auto mixed = digest(EVP_sha256(), hash2->data(), hash2->size(), scramble.data(), scramble.size());
    if (!mixed)
        return std::unexpected(mixed.error());
    xor_bytes(*hash1, *mixed);
    return std::move(*hash1);
}

bool same_bytes(const std::vector<uint8_t> & expected, const std::vector<uint8_t> & actual)
{
    if (expected.size() != actual.size())
    {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        diff = static_cast<unsigned char>(diff | (expected[i] ^ actual[i]));
    }
    return diff == 0;
}

} // namespace popo::server
