#include "server/server.h"
#include <thread>
#include <utility>
#include <spdlog/spdlog.h>
#include "server/session.h"

namespace popo::server
{
Server::Server(
    popo::catalog::SchemaManager & schema_mgr,
    popo::storage::StorageEngine & engine,
    AuthConfig auth,
    std::unique_ptr<AuthKeys> auth_keys)
    : acceptor_(io_context_)
    , schema_mgr_(schema_mgr)
    , engine_(engine)
    , auth_keys_(std::move(auth_keys))
    , auth_(std::move(auth))
    , accounts_(auth_.datadir, auth_.password, auth_.plugin)
{
    if (auth_.plugin.empty())
    {
        auth_.plugin = kCachingSha2;
    }
}

popo::base::Result<std::unique_ptr<Server>>
Server::create(uint16_t port, popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine, AuthConfig auth)
{
    auto keys = AuthKeys::create();
    if (!keys)
    {
        return std::unexpected(keys.error());
    }
    auto server = std::unique_ptr<Server>(new Server(schema_mgr, engine, std::move(auth), std::move(*keys)));
    if (!server->accounts_.status())
    {
        return std::unexpected(server->accounts_.status().error());
    }
    auto initialized = server->init(port);
    if (!initialized)
    {
        return std::unexpected(initialized.error());
    }
    return std::move(server);
}

popo::base::Result<void> Server::init(uint16_t port)
{
    std::error_code ec;
    asio::ip::address address;
    if (auth_.bind_address.empty() || auth_.bind_address == "0.0.0.0" || auth_.bind_address == "*")
    {
        address = asio::ip::address_v4::any();
    }
    else
    {
        address = asio::ip::make_address(auth_.bind_address, ec);
        if (ec)
        {
            return popo::base::make_error(popo::base::Errno::InvalidArg, "invalid bind address: " + ec.message());
        }
    }
    const asio::ip::tcp::endpoint endpoint(address, port);
    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to open listener: " + ec.message());
    }
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to configure listener: " + ec.message());
    }
    acceptor_.bind(endpoint, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to bind listener: " + ec.message());
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to listen: " + ec.message());
    }
    return {};
}

popo::base::Result<void> Server::start()
{
    running_ = true;
    auto listening_port = port();
    if (!listening_port)
    {
        return std::unexpected(listening_port.error());
    }
    spdlog::info("PopoDB server listening on port {}", *listening_port);
    do_accept();
    io_context_.run();
    return {};
}

popo::base::Result<uint16_t> Server::port() const
{
    std::error_code ec;
    const auto endpoint = acceptor_.local_endpoint(ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to read listener endpoint: " + ec.message());
    }
    return endpoint.port();
}

void Server::stop()
{
    running_ = false;
    // Close acceptor to prevent new connections
    std::error_code ec;
    acceptor_.close(ec);
    io_context_.stop();
}

void Server::do_accept()
{
    acceptor_.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket)
        {
            if (!ec && running_)
            {
                uint32_t conn_id = next_connection_id_++;
                std::error_code endpoint_ec;
                auto endpoint = socket.remote_endpoint(endpoint_ec);
                if (!endpoint_ec)
                {
                    spdlog::info("New connection #{} from {}", conn_id, endpoint.address().to_string());
                }
                auto session =
                    std::make_shared<Session>(std::move(socket), schema_mgr_, engine_, conn_id, *auth_keys_, auth_, accounts_);
                std::thread(
                    [session]()
                    {
                        auto result = session->start();
                        if (!result)
                        {
                            spdlog::error("Session error: {}", result.error().error_message);
                        }
                    })
                    .detach();
            }
            if (running_)
            {
                do_accept();
            }
        });
}

} // namespace popo::server
