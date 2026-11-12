#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <spdlog/spdlog.h>
#include "base/error.h"
#include "catalog/schema_mgr.h"
#include "server/config.h"
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
    spdlog::set_level(spdlog::level::info);
    std::string defaults_file;
    bool explicit_file = false;
    std::optional<uint16_t> port_override;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg.starts_with("--defaults-file="))
        {
            defaults_file = arg.substr(std::string("--defaults-file=").size());
            explicit_file = true;
        }
        else
        {
            auto parsed_port = popo::base::parse_u16(arg);
            if (!parsed_port)
            {
                spdlog::error("Invalid port '{}': {}", arg, parsed_port.error().error_message);
                return 1;
            }
            port_override = *parsed_port;
        }
    }
    if (!explicit_file)
    {
        if (std::filesystem::exists("deploy/popodb.cnf"))
            defaults_file = "deploy/popodb.cnf";
        else if (std::filesystem::exists("popodb.cnf"))
            defaults_file = "popodb.cnf";
    }

    popo::server::ServerConfig config;
    if (!defaults_file.empty())
    {
        auto loaded = popo::server::load_server_config(defaults_file);
        if (!loaded)
        {
            spdlog::error("{}", loaded.error().error_message);
            return 1;
        }
        config = std::move(*loaded);
        spdlog::info("Loaded {}", defaults_file);
    }
    if (port_override)
        config.port = *port_override;
    if (const char * password = std::getenv("POPO_PASSWORD"))
        config.root_password = password;
    if (const char * plugin = std::getenv("POPO_AUTH_PLUGIN"))
        config.plugin = plugin;
    if (std::getenv("POPO_AUTH_FULL") != nullptr)
        config.require_full_auth = true;

    spdlog::info("Starting PopoDB Database Server...");
    popo::catalog::SchemaManager schema_mgr;
    popo::storage::StorageEngine engine(config.datadir);
    auto initialized = engine.init();
    if (!initialized)
    {
        spdlog::error("{}", initialized.error().error_message);
        return 1;
    }

    popo::server::AuthConfig auth;
    auth.password = config.root_password;
    auth.plugin = config.plugin;
    auth.require_full_auth = config.require_full_auth;
    auth.bind_address = config.bind_address;
    auth.datadir = config.datadir;
    auth.database = config.database;

    auto created = popo::server::Server::create(config.port, schema_mgr, engine, auth);
    if (!created)
    {
        spdlog::error("{}", created.error().error_message);
        engine.shutdown();
        return 1;
    }
    auto & server = **created;
    auto listening_port = server.port();
    if (!listening_port)
    {
        spdlog::error("{}", listening_port.error().error_message);
        engine.shutdown();
        return 1;
    }
    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    SPDLOG_INFO("PopoDB Database Server v0.1.0");
    SPDLOG_INFO("Listening on {}:{}", config.bind_address, *listening_port);
    SPDLOG_INFO("Auth plugins: caching_sha2_password, mysql_native_password, sha256_password");
    if (config.root_password.empty())
        SPDLOG_INFO("Connect with: mysql -h 127.0.0.1 -P {} -u root", *listening_port);
    else
        SPDLOG_INFO("Connect with: mysql -h 127.0.0.1 -P {} -u root -p", *listening_port);
    auto started = server.start();
    g_server = nullptr;
    if (!started)
    {
        spdlog::error("{}", started.error().error_message);
        engine.shutdown();
        return 1;
    }

    engine.shutdown();
    SPDLOG_INFO("Server stopped.");
    return 0;
}
