#pragma once
#include <memory>
#include <string>
#include <asio.hpp>
#include "base/error.h"
#include "bytecode/compiler.h"
#include "bytecode/txn_session.h"
#include "bytecode/vm.h"
#include "catalog/schema_mgr.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "server/accounts.h"
#include "server/auth.h"
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
        uint32_t connection_id,
        const AuthKeys & auth_keys,
        AuthConfig auth,
        AccountStore & accounts);
    popo::base::Result<void> start();

private:
    asio::ip::tcp::socket socket_;
    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    uint32_t connection_id_;
    const AuthKeys & auth_keys_;
    AuthConfig auth_;
    AccountStore & accounts_;
    std::string current_user_;
    std::string account_host_;
    std::string client_host_;
    std::string current_db_;
    std::string login_password_;
    std::string requested_database_;
    std::vector<uint8_t> scramble_;
    uint8_t seq_id_ = 0;
    std::vector<uint8_t> read_buffer_;
    popo::bytecode::TxnSession txn_session_;

    // Transaction control is session-scoped, so it never reaches the planner.
    popo::base::Result<bool> handle_transaction_statement(const popo::parser::Statement & stmt);
    popo::base::Result<bool> handle_set_var(const popo::parser::SetVarStatement & stmt);
    popo::base::Result<void> apply_isolation(popo::parser::IsolationLevel level, popo::parser::TxnScope scope);

    popo::base::Result<void> do_handshake();
    popo::base::Result<void> read_handshake_response();
    popo::base::Result<std::vector<uint8_t>> read_payload(uint8_t & seq);
    popo::base::Result<void> send_bytes(uint8_t & seq, const std::vector<uint8_t> & payload);
    popo::base::Result<bool>
    finish_auth(const std::string & plugin, std::vector<uint8_t> data, uint8_t seq, const std::string & user);
    bool password_matches(const std::string & got) const;
    popo::base::Result<void> deny_auth(uint8_t seq, const std::string & user);
    popo::base::Result<void> command_loop();
    popo::base::Result<void> read_packet();
    popo::base::Result<void> handle_command(const uint8_t * payload, size_t length);
    popo::base::Result<void> handle_query(const std::string & sql);
    popo::base::Result<void> handle_ping();
    popo::base::Result<void> handle_init_db(const std::string & db_name);

    popo::base::Result<void> send_packet(const std::vector<uint8_t> & packet);
    popo::base::Result<void> send_ok(uint64_t affected_rows = 0, const std::string & msg = "");
    popo::base::Result<void> send_error(uint16_t code, const std::string & msg);
    popo::base::Result<void> send_result_set(const popo::bytecode::ExecuteSuccess & result);

    popo::base::Result<std::vector<uint8_t>> read_bytes(size_t n);
    MySQLColumnType value_type_to_mysql(popo::type::ValueType vt);
    std::string value_to_string(const popo::type::Value & val);
    uint32_t column_length_for_type(popo::type::ValueType vt);
};

} // namespace popo::server
