#include "server/server.h"
#include <thread>
#include <spdlog/spdlog.h>
#include "server/session.h"

namespace popo::server
{

Server::Server(uint16_t port, popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine)
    : acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)), schema_mgr_(schema_mgr), engine_(engine)
{
}

void Server::start()
{
    running_ = true;
    spdlog::info("PopoDB server listening on port {}", acceptor_.local_endpoint().port());
    do_accept();
    io_context_.run();
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
                spdlog::info("New connection #{} from {}", conn_id, socket.remote_endpoint().address().to_string());
                auto session = std::make_shared<Session>(std::move(socket), schema_mgr_, engine_, conn_id);
                std::thread(
                    [session]()
                    {
                        try
                        {
                            session->start();
                        }
                        catch (const std::exception & e)
                        {
                            spdlog::error("Session error: {}", e.what());
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
