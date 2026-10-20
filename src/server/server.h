#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <asio.hpp>
#include "catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"

namespace popo::server
{

class Server
{
public:
    Server(uint16_t port, popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine);
    void start();
    void stop();

private:
    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    std::atomic<uint32_t> next_connection_id_{1};
    std::atomic<bool> running_{false};
    void do_accept();
};

} // namespace popo::server
