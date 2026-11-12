#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <asio.hpp>
#include "base/error.h"
#include "catalog/schema_mgr.h"
#include "server/accounts.h"
#include "server/auth.h"
#include "src/storage/storage_engine.h"

namespace popo::server
{

class Server
{
public:
    static popo::base::Result<std::unique_ptr<Server>> create(
        uint16_t port,
        popo::catalog::SchemaManager & schema_mgr,
        popo::storage::StorageEngine & engine,
        AuthConfig auth = {});
    popo::base::Result<void> start();
    void stop();
    popo::base::Result<uint16_t> port() const;

private:
    Server(
        popo::catalog::SchemaManager & schema_mgr,
        popo::storage::StorageEngine & engine,
        AuthConfig auth,
        std::unique_ptr<AuthKeys> auth_keys);

    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    std::unique_ptr<AuthKeys> auth_keys_;
    AuthConfig auth_;
    AccountStore accounts_;
    std::atomic<uint32_t> next_connection_id_{1};
    std::atomic<bool> running_{false};
    popo::base::Result<void> init(uint16_t port);
    void do_accept();
};

} // namespace popo::server
