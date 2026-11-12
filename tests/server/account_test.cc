#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "parser/parser.h"
#include "server/accounts.h"
#include "server/auth.h"
#include "server/config.h"
#include "server/protocol.h"
#include "server/server.h"
#include "src/storage/storage_engine.h"

namespace
{

popo::server::AccountReply
run_sql(popo::server::AccountStore & store, const popo::server::AccountActor & actor, const std::string & sql)
{
    popo::parser::Parser parser;
    auto parsed = parser.parse(sql);
    EXPECT_TRUE(static_cast<bool>(parsed)) << sql;
    if (!parsed)
    {
        popo::server::AccountReply reply;
        reply.ok = false;
        reply.message = "parse failed";
        return reply;
    }
    if (popo::server::is_account_statement(parsed->front()->type))
    {
        return store.execute(*parsed->front(), actor);
    }
    return store.authorize(*parsed->front(), actor);
}

TEST(AccountStore, HostMatchGrantAndReload)
{
    auto dir = std::filesystem::temp_directory_path() / "popo_accounts_unit";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    popo::server::AccountActor root{"root", "%", "127.0.0.1", "popo"};
    {
        popo::server::AccountStore store(dir.string(), "secret", popo::server::kCachingSha2);
        ASSERT_TRUE(store.status()) << store.status().error().error_message;
        auto created = run_sql(
            store, root, "CREATE USER 'root'@'127.0.0.1' IDENTIFIED BY 'local', 'bob'@'%' IDENTIFIED BY 'bobpass'");
        ASSERT_TRUE(created.ok) << created.message;
        auto again = run_sql(store, root, "CREATE USER IF NOT EXISTS 'bob'@'%' IDENTIFIED BY 'other'");
        ASSERT_TRUE(again.ok) << again.message;
        auto granted = run_sql(store, root, "GRANT SELECT ON popo.* TO 'bob'@'%'");
        ASSERT_TRUE(granted.ok) << granted.message;
        auto show = run_sql(store, root, "SHOW GRANTS FOR 'bob'@'%'");
        ASSERT_TRUE(show.ok) << show.message;
        ASSERT_FALSE(show.rows.empty());
        EXPECT_NE(show.rows[0].find("GRANT SELECT ON popo.* TO 'bob'@'%'"), std::string::npos);

        std::string host;
        auto local = store.lookup("root", "127.0.0.1", host);
        ASSERT_TRUE(local);
        EXPECT_EQ(*local, "local");
        EXPECT_EQ(host, "127.0.0.1");
        auto remote = store.lookup("root", "10.0.0.8", host);
        ASSERT_TRUE(remote);
        EXPECT_EQ(*remote, "secret");
        EXPECT_EQ(host, "%");
        EXPECT_FALSE(store.lookup("nobody", "127.0.0.1", host));

        popo::server::AccountActor bob{"bob", "%", "10.0.0.8", "popo"};
        auto denied = run_sql(store, bob, "CREATE USER 'eve'@'%' IDENTIFIED BY 'x'");
        EXPECT_FALSE(denied.ok);
        EXPECT_EQ(denied.code, 1227);
        EXPECT_FALSE(store.has_priv("bob", "%", popo::parser::kPrivInsert, "popo", "t"));
        EXPECT_TRUE(store.has_priv("bob", "%", popo::parser::kPrivSelect, "popo", "t"));
        auto select_ok = run_sql(store, bob, "SELECT id FROM t");
        EXPECT_TRUE(select_ok.ok) << select_ok.message;
        auto insert_no = run_sql(store, bob, "INSERT INTO t VALUES (1)");
        EXPECT_FALSE(insert_no.ok);
        EXPECT_EQ(insert_no.code, 1142);
    }
    {
        popo::server::AccountStore loaded(dir.string(), "ignored", popo::server::kCachingSha2);
        ASSERT_TRUE(loaded.status()) << loaded.status().error().error_message;
        std::string host;
        auto local = loaded.lookup("root", "127.0.0.1", host);
        ASSERT_TRUE(local);
        EXPECT_EQ(*local, "local");
        EXPECT_TRUE(loaded.has_priv("bob", "%", popo::parser::kPrivSelect, "popo", "emp"));
        auto revoked = run_sql(loaded, root, "REVOKE SELECT ON popo.* FROM 'bob'@'%'");
        ASSERT_TRUE(revoked.ok) << revoked.message;
        EXPECT_FALSE(loaded.has_priv("bob", "%", popo::parser::kPrivSelect, "popo", "emp"));
        auto changed = run_sql(loaded, root, "SET PASSWORD FOR 'bob'@'%' = 'newpass'");
        ASSERT_TRUE(changed.ok) << changed.message;
        auto password = loaded.lookup("bob", "10.1.1.1", host);
        ASSERT_TRUE(password);
        EXPECT_EQ(*password, "newpass");
    }
    std::filesystem::remove_all(dir);
}

TEST(ServerConfig, ReadsKeys)
{
    auto path = std::filesystem::temp_directory_path() / "popodb-test.cnf";
    {
        std::ofstream out(path);
        out << "# comment\n[popodb]\nport=3310\nbind-address=127.0.0.1\ndatadir=/tmp/popo\n"
            << "default-auth-plugin=mysql_native_password\nroot-password=secret\nrequire-full-auth=1\n";
    }
    auto config = popo::server::load_server_config(path.string());
    ASSERT_TRUE(config) << config.error().error_message;
    EXPECT_EQ(config->port, 3310);
    EXPECT_EQ(config->bind_address, "127.0.0.1");
    EXPECT_EQ(config->datadir, "/tmp/popo");
    EXPECT_EQ(config->plugin, "mysql_native_password");
    EXPECT_EQ(config->root_password, "secret");
    EXPECT_TRUE(config->require_full_auth);
    std::filesystem::remove(path);
}

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
            ::close(fd_);
    }
    std::vector<uint8_t> read_payload()
    {
        uint8_t header[4];
        if (!read_full(header, 4))
            return {};
        uint32_t len = header[0] | (static_cast<uint32_t>(header[1]) << 8) | (static_cast<uint32_t>(header[2]) << 16);
        std::vector<uint8_t> payload(len);
        if (len > 0 && !read_full(payload.data(), len))
            return {};
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

std::vector<uint8_t> handshake_response(
    const std::string & user,
    const std::vector<uint8_t> & auth,
    const std::string & plugin = popo::server::kCachingSha2)
{
    using popo::server::Protocol;
    uint32_t caps = popo::server::CLIENT_PROTOCOL_41 | popo::server::CLIENT_SECURE_CONNECTION
        | popo::server::CLIENT_PLUGIN_AUTH | popo::server::CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA;
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
    if (payload.empty())
    {
        ADD_FAILURE() << "empty handshake";
        return {};
    }
    size_t pos = 1;
    while (pos < payload.size() && payload[pos] != 0)
        ++pos;
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
        --rest;
    scramble.insert(
        scramble.end(),
        payload.begin() + static_cast<std::ptrdiff_t>(pos),
        payload.begin() + static_cast<std::ptrdiff_t>(pos + rest));
    return scramble;
}

void login(MysqlClient & client, const std::string & user, const std::string & password)
{
    auto scramble = scramble_from_handshake(client.read_payload());
    ASSERT_EQ(scramble.size(), 20u);
    auto token = popo::server::caching_sha2_token(password, scramble);
    ASSERT_TRUE(token) << token.error().error_message;
    client.write_payload(1, handshake_response(user, *token));
    auto more = client.read_payload();
    ASSERT_EQ(more.size(), 2u);
    ASSERT_EQ(more[0], 0x01);
    ASSERT_EQ(more[1], 0x03);
    auto ok = client.read_payload();
    ASSERT_FALSE(ok.empty());
    ASSERT_EQ(ok[0], 0x00);
}

struct QueryResult
{
    std::vector<uint8_t> first;
    std::string text;
};

QueryResult query(MysqlClient & client, const std::string & sql)
{
    std::vector<uint8_t> packet{0x03};
    packet.insert(packet.end(), sql.begin(), sql.end());
    client.write_payload(0, packet);
    QueryResult result;
    int eofs = 0;
    for (int i = 0; i < 32; ++i)
    {
        auto payload = client.read_payload();
        if (i == 0)
            result.first = payload;
        result.text.append(reinterpret_cast<const char *>(payload.data()), payload.size());
        if (!payload.empty() && payload[0] == 0xFF)
            break;
        if (!payload.empty() && payload[0] == 0x00 && i == 0)
            break;
        if (!payload.empty() && payload[0] == 0xFE && payload.size() < 9 && ++eofs == 2)
            break;
    }
    return result;
}

uint16_t error_code(const std::vector<uint8_t> & payload)
{
    return static_cast<uint16_t>(payload[1] | (payload[2] << 8));
}

TEST(AccountLogin, CreateUserGrantAndDeny)
{
    auto dir = std::filesystem::temp_directory_path() / "popo_accounts_live";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    popo::catalog::SchemaManager schema;
    popo::storage::StorageEngine engine(dir.string());
    auto initialized = engine.init();
    ASSERT_TRUE(initialized) << initialized.error().error_message;
    popo::server::AuthConfig auth;
    auth.password = "secret";
    auth.datadir = dir.string();
    auto server_result = popo::server::Server::create(0, schema, engine, auth);
    ASSERT_TRUE(server_result) << server_result.error().error_message;
    auto server = std::move(*server_result);
    auto port_result = server->port();
    ASSERT_TRUE(port_result) << port_result.error().error_message;
    const uint16_t port = *port_result;
    std::thread thread(
        [server_ptr = server.get()]
        {
            auto started = server_ptr->start();
            EXPECT_TRUE(started) << (started ? "" : started.error().error_message);
        });
    struct Stop
    {
        popo::server::Server & server;
        std::thread & thread;
        popo::storage::StorageEngine & engine;
        std::filesystem::path dir;
        ~Stop()
        {
            server.stop();
            if (thread.joinable())
                thread.join();
            engine.shutdown();
            std::filesystem::remove_all(dir);
        }
    } stop{*server, thread, engine, dir};
    MysqlClient root(port);
    login(root, "root", "secret");
    auto created = query(root, "CREATE USER 'bob'@'%' IDENTIFIED BY 'bobpass'");
    ASSERT_FALSE(created.first.empty());
    ASSERT_EQ(created.first[0], 0x00) << created.text;
    auto granted = query(root, "GRANT SELECT ON popo.* TO 'bob'@'%'");
    ASSERT_EQ(granted.first[0], 0x00) << granted.text;
    auto grants = query(root, "SHOW GRANTS FOR 'bob'@'%'");
    EXPECT_NE(grants.text.find("GRANT SELECT ON popo.*"), std::string::npos) << grants.text;

    MysqlClient bob(port);
    login(bob, "bob", "bobpass");
    auto selected = query(bob, "SELECT 1");
    ASSERT_FALSE(selected.first.empty());
    EXPECT_NE(selected.first[0], 0xFF) << selected.text;
    auto blocked = query(bob, "CREATE TABLE t (id INT)");
    ASSERT_FALSE(blocked.first.empty());
    ASSERT_EQ(blocked.first[0], 0xFF);
    EXPECT_EQ(error_code(blocked.first), 1142);

    MysqlClient bad(port);
    auto scramble = scramble_from_handshake(bad.read_payload());
    ASSERT_EQ(scramble.size(), 20u);
    auto bad_token = popo::server::native_password_token("wrong", scramble);
    ASSERT_TRUE(bad_token) << bad_token.error().error_message;
    bad.write_payload(1, handshake_response("bob", *bad_token, popo::server::kNativePassword));
    auto err = bad.read_payload();
    ASSERT_FALSE(err.empty());
    EXPECT_EQ(err[0], 0xFF);
    EXPECT_EQ(error_code(err), 1045);
}

} // namespace
