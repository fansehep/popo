#pragma once
#include "catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"
#include <asio.hpp>
#include <memory>
#include <atomic>
#include <cstdint>

namespace neon::server {

class Server {
public:
    Server(uint16_t port, neon::catalog::SchemaManager& schema_mgr, neon::storage::StorageEngine& engine);
    void start();
    void stop();

private:
    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    neon::catalog::SchemaManager& schema_mgr_;
    neon::storage::StorageEngine& engine_;
    std::atomic<uint32_t> next_connection_id_{1};
    std::atomic<bool> running_{false};
    void do_accept();
};

} // namespace neon::server
