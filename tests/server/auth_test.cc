#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "server/auth.h"
#include "server/protocol.h"
#include "server/server.h"
#include "src/storage/storage_engine.h"

namespace
{

class MysqlClient
{
public:
    explicit MysqlClient(uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
        {
            ADD_FAILURE() << "socket failed";
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ADD_FAILURE() << "connect failed";
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~MysqlClient()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    std::vector<uint8_t> read_payload()
    {
        uint8_t header[4];
        if (!read_full(header, 4))
        {
            return {};
        }
        uint32_t len = header[0] | (static_cast<uint32_t>(header[1]) << 8) | (static_cast<uint32_t>(header[2]) << 16);
        std::vector<uint8_t> payload(len);
        if (len > 0 && !read_full(payload.data(), len))
        {
            return {};
        }
        return payload;
    }

    void write_payload(uint8_t seq, const std::vector<uint8_t> & payload)
    {
        auto packet_result = popo::server::Protocol::make_packet(seq, payload);
        if (!packet_result)
        {
            ADD_FAILURE() << "make_packet failed: " << packet_result.error().error_message;
            return;
        }
        const auto & packet = *packet_result;
        size_t sent = 0;
        while (sent < packet.size())
        {
            ssize_t n = ::send(fd_, packet.data() + sent, packet.size() - sent, 0);
            if (n <= 0)
            {
                ADD_FAILURE() << "send failed";
                return;
            }
            sent += static_cast<size_t>(n);
        }
    }

private:
    bool read_full(uint8_t * dst, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            ssize_t r = ::recv(fd_, dst + got, n - got, 0);
            if (r <= 0)
            {
                ADD_FAILURE() << "recv failed";
                return false;
            }
            got += static_cast<size_t>(r);
        }
        return true;
    }

    int fd_ = -1;
};

std::vector<uint8_t>
handshake_response(const std::string & user, const std::vector<uint8_t> & auth, const std::string & plugin)
{
    using popo::server::Protocol;
    uint32_t caps = popo::server::CLIENT_LONG_PASSWORD | popo::server::CLIENT_PROTOCOL_41
        | popo::server::CLIENT_SECURE_CONNECTION | popo::server::CLIENT_PLUGIN_AUTH
        | popo::server::CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | popo::server::CLIENT_TRANSACTIONS
        | popo::server::CLIENT_MULTI_RESULTS;
    std::vector<uint8_t> payload;
    Protocol::write_int4(payload, caps);
    Protocol::write_int4(payload, 16777216);
    payload.push_back(45);
    payload.insert(payload.end(), 23, 0);
    Protocol::write_null_string(payload, user);
    Protocol::write_lenenc_string(payload, std::string(auth.begin(), auth.end()));
    Protocol::write_null_string(payload, plugin);
    return payload;
}

std::vector<uint8_t> scramble_from_handshake(const std::vector<uint8_t> & payload)
{
    // protocol(1) + version cstr + conn id(4) + scramble part 1(8)
    if (payload.empty())
    {
        ADD_FAILURE() << "empty handshake";
        return {};
    }
    size_t pos = 1;
    while (pos < payload.size() && payload[pos] != 0)
    {
        ++pos;
    }
    if (pos == payload.size())
    {
        ADD_FAILURE() << "unterminated server version";
        return {};
    }
    ++pos;
    pos += 4;
    if (pos + 8 > payload.size())
    {
        ADD_FAILURE() << "short handshake";
        return {};
    }
    std::vector<uint8_t> scramble(
        payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.begin() + static_cast<std::ptrdiff_t>(pos + 8));
    pos += 8 + 1 + 2 + 1 + 2 + 2;
    if (pos >= payload.size())
    {
        ADD_FAILURE() << "handshake missing auth length";
        return {};
    }
    uint8_t auth_len = payload[pos];
    ++pos;
    pos += 10;
    size_t rest = auth_len > 8 ? static_cast<size_t>(auth_len - 8) : 0;
    if (pos > payload.size() || rest > payload.size() - pos)
    {
        ADD_FAILURE() << "short handshake auth data";
        return {};
    }
    if (rest > 0 && payload[pos + rest - 1] == 0)
    {
        --rest;
    }
    scramble.insert(
        scramble.end(),
        payload.begin() + static_cast<std::ptrdiff_t>(pos),
        payload.begin() + static_cast<std::ptrdiff_t>(pos + rest));
    return scramble;
}

std::vector<uint8_t>
rsa_encrypt(const std::string & pem, const std::string & password, const std::vector<uint8_t> & scramble)
{
    BIO * bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr)
    {
        ADD_FAILURE() << "public key BIO allocation failed";
        return {};
    }
    EVP_PKEY * key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr)
    {
        ADD_FAILURE() << "public key parse failed";
        return {};
    }
    if (scramble.empty())
    {
        ADD_FAILURE() << "empty scramble";
        EVP_PKEY_free(key);
        return {};
    }
    std::vector<uint8_t> plain(password.size() + 1);
    std::memcpy(plain.data(), password.data(), password.size());
    for (size_t i = 0; i < plain.size(); ++i)
    {
        plain[i] = static_cast<uint8_t>(plain[i] ^ scramble[i % scramble.size()]);
    }
    EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new(key, nullptr);
    if (ctx == nullptr || EVP_PKEY_encrypt_init(ctx) != 1
        || EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1
        || EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha1()) != 1 || EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha1()) != 1)
    {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        ADD_FAILURE() << "encrypt init failed";
        return {};
    }
    size_t out_len = 0;
    if (EVP_PKEY_encrypt(ctx, nullptr, &out_len, plain.data(), plain.size()) != 1)
    {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        ADD_FAILURE() << "encrypt size failed";
        return {};
    }
    std::vector<uint8_t> out(out_len);
    if (EVP_PKEY_encrypt(ctx, out.data(), &out_len, plain.data(), plain.size()) != 1)
    {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        ADD_FAILURE() << "encrypt failed";
        return {};
    }
    out.resize(out_len);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    return out;
}

class AuthServer
{
public:
    static popo::base::Result<std::unique_ptr<AuthServer>> create(popo::server::AuthConfig auth)
    {
        auto instance = std::unique_ptr<AuthServer>(new AuthServer());
        auto initialized = instance->engine_.init();
        if (!initialized)
        {
            return std::unexpected(initialized.error());
        }
        auto server = popo::server::Server::create(0, instance->schema_, instance->engine_, std::move(auth));
        if (!server)
        {
            return std::unexpected(server.error());
        }
        instance->server_ = std::move(*server);
        auto port = instance->server_->port();
        if (!port)
        {
            return std::unexpected(port.error());
        }
        instance->port_ = *port;
        instance->thread_ = std::thread(
            [server = instance->server_.get()]
            {
                auto started = server->start();
                EXPECT_TRUE(started) << (started ? "" : started.error().error_message);
            });
        return instance;
    }

    ~AuthServer()
    {
        if (server_)
        {
            server_->stop();
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        engine_.shutdown();
        std::filesystem::remove_all(dir_);
    }

    uint16_t port() const { return port_; }

private:
    AuthServer()
        : dir_(std::filesystem::temp_directory_path() / ("popo_auth_" + std::to_string(std::random_device{}())))
        , engine_(dir_.string())
    {
        std::filesystem::create_directories(dir_);
    }

    std::filesystem::path dir_;
    popo::catalog::SchemaManager schema_;
    popo::storage::StorageEngine engine_;
    std::unique_ptr<popo::server::Server> server_;
    std::thread thread_;
    uint16_t port_ = 0;
};

void expect_ok(const std::vector<uint8_t> & payload)
{
    ASSERT_FALSE(payload.empty());
    ASSERT_EQ(payload[0], 0x00) << "expected OK, got " << static_cast<int>(payload[0]);
}

} // namespace

TEST(AuthLogin, CachingSha2FastPathAndSystemVars)
{
    popo::server::AuthConfig auth;
    auth.password = "secret";
    auto server_result = AuthServer::create(auth);
    ASSERT_TRUE(server_result) << server_result.error().error_message;
    auto server = std::move(*server_result);
    MysqlClient client(server->port());
    auto hello = client.read_payload();
    auto scramble = scramble_from_handshake(hello);
    ASSERT_EQ(scramble.size(), 20u);
    auto token = popo::server::caching_sha2_token(auth.password, scramble);
    ASSERT_TRUE(token) << token.error().error_message;
    client.write_payload(1, handshake_response("root", *token, popo::server::kCachingSha2));
    auto more = client.read_payload();
    ASSERT_EQ(more.size(), 2u);
    EXPECT_EQ(more[0], 0x01);
    EXPECT_EQ(more[1], 0x03);
    expect_ok(client.read_payload());

    std::string sql = "SELECT @@version, @@session.auto_increment_increment AS auto_increment_increment";
    std::vector<uint8_t> query{0x03};
    query.insert(query.end(), sql.begin(), sql.end());
    client.write_payload(0, query);
    bool saw_version = false;
    for (int i = 0; i < 8; ++i)
    {
        auto pkt = client.read_payload();
        std::string text(pkt.begin(), pkt.end());
        if (text.find("5.7.42-popodb") != std::string::npos)
        {
            saw_version = true;
            break;
        }
    }
    EXPECT_TRUE(saw_version);
}

TEST(AuthLogin, CachingSha2RsaFullAuth)
{
    popo::server::AuthConfig auth;
    auth.password = "secret";
    auth.require_full_auth = true;
    auto server_result = AuthServer::create(auth);
    ASSERT_TRUE(server_result) << server_result.error().error_message;
    auto server = std::move(*server_result);
    MysqlClient client(server->port());
    auto scramble = scramble_from_handshake(client.read_payload());
    auto token = popo::server::caching_sha2_token(auth.password, scramble);
    ASSERT_TRUE(token) << token.error().error_message;
    client.write_payload(1, handshake_response("root", *token, popo::server::kCachingSha2));
    auto more = client.read_payload();
    ASSERT_EQ(more.size(), 2u);
    EXPECT_EQ(more[1], 0x04);
    client.write_payload(3, {0x02});
    auto key_pkt = client.read_payload();
    ASSERT_FALSE(key_pkt.empty());
    EXPECT_EQ(key_pkt[0], 0x01);
    std::string pem(key_pkt.begin() + 1, key_pkt.end());
    auto cipher = rsa_encrypt(pem, auth.password, scramble);
    client.write_payload(5, cipher);
    expect_ok(client.read_payload());
}

TEST(AuthLogin, NativePasswordAndSha256Key)
{
    popo::server::AuthConfig auth;
    auth.password = "secret";
    auto server_result = AuthServer::create(auth);
    ASSERT_TRUE(server_result) << server_result.error().error_message;
    auto server = std::move(*server_result);
    {
        MysqlClient client(server->port());
        auto scramble = scramble_from_handshake(client.read_payload());
        auto token = popo::server::native_password_token(auth.password, scramble);
        ASSERT_TRUE(token) << token.error().error_message;
        client.write_payload(1, handshake_response("root", *token, popo::server::kNativePassword));
        expect_ok(client.read_payload());
    }
    {
        MysqlClient client(server->port());
        auto scramble = scramble_from_handshake(client.read_payload());
        client.write_payload(1, handshake_response("root", {0x01}, popo::server::kSha256Password));
        auto key_pkt = client.read_payload();
        ASSERT_GE(key_pkt.size(), 2u);
        EXPECT_EQ(key_pkt[0], 0x01);
        std::string pem(key_pkt.begin() + 1, key_pkt.end());
        auto cipher = rsa_encrypt(pem, auth.password, scramble);
        client.write_payload(3, cipher);
        expect_ok(client.read_payload());
    }
}

TEST(AuthLogin, RejectsWrongPasswordAndAcceptsEmpty)
{
    {
        popo::server::AuthConfig auth;
        auth.password = "secret";
        auto server_result = AuthServer::create(auth);
        ASSERT_TRUE(server_result) << server_result.error().error_message;
        auto server = std::move(*server_result);
        MysqlClient client(server->port());
        auto scramble = scramble_from_handshake(client.read_payload());
        auto token = popo::server::native_password_token("nope", scramble);
        ASSERT_TRUE(token) << token.error().error_message;
        client.write_payload(1, handshake_response("root", *token, popo::server::kNativePassword));
        auto err = client.read_payload();
        ASSERT_FALSE(err.empty());
        EXPECT_EQ(err[0], 0xFF);
    }
    {
        auto server_result = AuthServer::create({});
        ASSERT_TRUE(server_result) << server_result.error().error_message;
        auto server = std::move(*server_result);
        MysqlClient client(server->port());
        client.read_payload();
        client.write_payload(1, handshake_response("root", {}, popo::server::kCachingSha2));
        expect_ok(client.read_payload());
    }
}

TEST(AuthLogin, SwitchesUnknownPluginToCachingSha2)
{
    popo::server::AuthConfig auth;
    auth.password = "secret";
    auto server_result = AuthServer::create(auth);
    ASSERT_TRUE(server_result) << server_result.error().error_message;
    auto server = std::move(*server_result);
    MysqlClient client(server->port());
    auto scramble = scramble_from_handshake(client.read_payload());
    client.write_payload(1, handshake_response("root", {}, "mysql_old_password"));
    auto switched = client.read_payload();
    ASSERT_FALSE(switched.empty());
    EXPECT_EQ(switched[0], 0xFE);
    auto token = popo::server::caching_sha2_token(auth.password, scramble);
    ASSERT_TRUE(token) << token.error().error_message;
    client.write_payload(3, *token);
    auto more = client.read_payload();
    ASSERT_EQ(more.size(), 2u);
    EXPECT_EQ(more[1], 0x03);
    expect_ok(client.read_payload());
}
