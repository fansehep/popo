#include <csignal>
#include <iostream>
#include <spdlog/spdlog.h>
#include "catalog/schema_mgr.h"
#include "server/server.h"
#include "src/storage/storage_engine.h"

static popo::server::Server * g_server = nullptr;

void signal_handler(int sig)
{
    SPDLOG_INFO("Received signal {}, shutting down...", sig);
    if (g_server)
    {
        g_server->stop();
    }
}

int main(int argc, char * argv[])
{
    uint16_t port = 3307;
    if (argc > 1)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting PopoDB Database Server...");
    popo::catalog::SchemaManager schema_mgr;

    // Initialize storage engine
    popo::storage::StorageEngine engine("data");
    engine.init();

    popo::server::Server server(port, schema_mgr, engine);
    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    SPDLOG_INFO("PopoDB Database Server v0.1.0");
    SPDLOG_INFO("Listening on port {}", port);
    SPDLOG_INFO("Connect with: mysql -h 127.0.0.1 -P {} -u root", port);
    server.start();

    // Shutdown storage engine
    engine.shutdown();
    SPDLOG_INFO("Server stopped.");
    return 0;
}
