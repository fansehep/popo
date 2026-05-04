#include "server/server.h"
#include "catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"
#include <spdlog/spdlog.h>
#include <csignal>
#include <iostream>

static neon::server::Server* g_server = nullptr;

void signal_handler(int sig) {
    SPDLOG_INFO("Received signal {}, shutting down...", sig);
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 3307;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    spdlog::info("Starting Neon Database Server...");
    neon::catalog::SchemaManager schema_mgr;
    
    // Initialize storage engine
    neon::storage::StorageEngine engine("data");
    engine.init();
    
    neon::server::Server server(port, schema_mgr, engine);
    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    SPDLOG_INFO("Neon Database Server v0.1.0");
    SPDLOG_INFO("Listening on port {}", port);
    SPDLOG_INFO("Connect with: mysql -h 127.0.0.1 -P {} -u root", port);
    server.start();
    
    // Shutdown storage engine
    engine.shutdown();
    SPDLOG_INFO("Server stopped.");
    return 0;
}
