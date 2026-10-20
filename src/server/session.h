#pragma once
#include <memory>
#include <string>
#include <asio.hpp>
#include "bytecode/compiler.h"
#include "bytecode/vm.h"
#include "catalog/schema_mgr.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "server/protocol.h"
#include "src/storage/storage_engine.h"

namespace popo::server
{

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(
        asio::ip::tcp::socket socket,
        popo::catalog::SchemaManager & schema_mgr,
        popo::storage::StorageEngine & engine,
        uint32_t connection_id);
    void start();

private:
    asio::ip::tcp::socket socket_;
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    uint32_t connection_id_;
    uint8_t seq_id_ = 0;
    std::vector<uint8_t> read_buffer_;

    void do_handshake();
    void read_handshake_response();
    void command_loop();
    void read_packet();
    void handle_command(const uint8_t * payload, size_t length);
    void handle_query(const std::string & sql);
    void handle_ping();
    void handle_init_db(const std::string & db_name);

    void send_packet(const std::vector<uint8_t> & packet);
    void send_ok(uint64_t affected_rows = 0, const std::string & msg = "");
    void send_error(uint16_t code, const std::string & msg);
    void send_result_set(const popo::bytecode::ExecuteSuccess & result);

    std::vector<uint8_t> read_bytes(size_t n);
    MySQLColumnType value_type_to_mysql(popo::type::ValueType vt);
    std::string value_to_string(const popo::type::Value & val);
    uint32_t column_length_for_type(popo::type::ValueType vt);
};

} // namespace popo::server
