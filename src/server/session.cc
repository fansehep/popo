#include "server/session.h"
#include <cctype>
#include <chrono>
#include <format>
#include <spdlog/spdlog.h>
#include "bytecode/fast_path.h"
#include "executor/executor.h"
#include "server/protocol.h"

namespace popo::server
{

Session::Session(
    asio::ip::tcp::socket socket,
    popo::catalog::SchemaManager & schema_mgr,
    popo::storage::StorageEngine & engine,
    uint32_t connection_id,
    const AuthKeys & auth_keys,
    AuthConfig auth,
    AccountStore & accounts)
    : socket_(std::move(socket))
    , schema_mgr_(schema_mgr)
    , engine_(engine)
    , connection_id_(connection_id)
    , auth_keys_(auth_keys)
    , auth_(std::move(auth))
    , accounts_(accounts)
    , current_db_(auth_.database.empty() ? "popo" : auth_.database)
    , txn_session_(engine)
{
}

popo::base::Result<void> Session::start()
{
    auto scramble = make_scramble();
    if (!scramble)
    {
        spdlog::warn("Session {} closed: {}", connection_id_, scramble.error().error_message);
        return std::unexpected(scramble.error());
    }
    scramble_ = std::move(*scramble);
    auto result = do_handshake();
    if (result)
    {
        result = read_handshake_response();
    }
    if (result)
    {
        result = command_loop();
    }
    if (!result)
    {
        spdlog::warn("Session {} closed: {}", connection_id_, result.error().error_message);
    }
    return result;
}

popo::base::Result<void> Session::do_handshake()
{
    auto packet = Protocol::build_handshake_v10(connection_id_, scramble_, auth_.plugin);
    if (!packet)
    {
        return std::unexpected(packet.error());
    }
    return send_packet(*packet);
}

popo::base::Result<std::vector<uint8_t>> Session::read_payload(uint8_t & seq)
{
    auto header_bytes = read_bytes(4);
    if (!header_bytes)
    {
        return std::unexpected(header_bytes.error());
    }
    auto header = Protocol::read_packet_header(header_bytes->data(), header_bytes->size());
    if (!header)
    {
        return std::unexpected(header.error());
    }
    if (header->payload_length > 1024 * 1024)
    {
        return popo::base::make_error(popo::base::Errno::ProtocolErr, "client packet is too large");
    }
    seq = header->sequence_id;
    return read_bytes(header->payload_length);
}

popo::base::Result<void> Session::send_bytes(uint8_t & seq, const std::vector<uint8_t> & payload)
{
    auto packet = Protocol::make_packet(seq, payload);
    if (!packet)
    {
        return std::unexpected(packet.error());
    }
    auto sent = send_packet(*packet);
    if (!sent)
    {
        return sent;
    }
    ++seq;
    return {};
}

bool Session::password_matches(const std::string & got) const
{
    return got == login_password_;
}

popo::base::Result<void> Session::deny_auth(uint8_t seq, const std::string & user)
{
    seq_id_ = seq;
    return send_error(1045, "Access denied for user '" + user + "'");
}

popo::base::Result<bool>
Session::finish_auth(const std::string & plugin, std::vector<uint8_t> data, uint8_t seq, const std::string & user)
{
    auto reject = [&]() -> popo::base::Result<bool>
    {
        auto denied = deny_auth(seq, user);
        if (!denied)
        {
            return std::unexpected(denied.error());
        }
        return false;
    };
    auto accept = [&]() -> popo::base::Result<bool>
    {
        if (!requested_database_.empty()
            && !accounts_.can_use_database(current_user_, account_host_, requested_database_))
        {
            seq_id_ = seq;
            auto sent = send_error(
                1044,
                "Access denied for user '" + user + "'@'" + client_host_ + "' to database '" + requested_database_
                    + "'");
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return false;
        }
        if (!requested_database_.empty())
        {
            current_db_ = requested_database_;
        }
        seq_id_ = seq;
        auto sent = send_ok();
        if (!sent)
        {
            return std::unexpected(sent.error());
        }
        spdlog::info("Connection {} authenticated user {} with {}", connection_id_, user, plugin);
        return true;
    };
    auto decrypted_matches = [&](const std::vector<uint8_t> & cipher) -> popo::base::Result<bool>
    {
        auto got = auth_keys_.decrypt_password(cipher.data(), cipher.size(), scramble_.data(), scramble_.size());
        if (!got)
        {
            if (got.error().error_code == popo::base::Errno::AuthDenied)
            {
                return false;
            }
            return std::unexpected(got.error());
        }
        return password_matches(*got);
    };

    if (plugin == kNativePassword)
    {
        if (login_password_.empty())
        {
            return data.empty() ? accept() : reject();
        }
        auto expect = native_password_token(login_password_, scramble_);
        if (!expect)
        {
            return std::unexpected(expect.error());
        }
        return same_bytes(*expect, data) ? accept() : reject();
    }
    if (plugin == kClearPassword)
    {
        std::string got(data.begin(), data.end());
        if (!got.empty() && got.back() == '\0')
        {
            got.pop_back();
        }
        return password_matches(got) ? accept() : reject();
    }
    if (plugin == kSha256Password)
    {
        if (login_password_.empty() && (data.empty() || (data.size() == 1 && data[0] == 0)))
        {
            return accept();
        }
        if (data.size() == 1 && data[0] == 1)
        {
            std::vector<uint8_t> pem(auth_keys_.public_pem().begin(), auth_keys_.public_pem().end());
            pem.push_back(0);
            auto sent = send_bytes(
                seq,
                [&]()
                {
                    std::vector<uint8_t> payload{0x01};
                    payload.insert(payload.end(), pem.begin(), pem.end());
                    return payload;
                }());
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            uint8_t client_seq = 0;
            auto payload = read_payload(client_seq);
            if (!payload)
            {
                return std::unexpected(payload.error());
            }
            data = std::move(*payload);
            seq = static_cast<uint8_t>(client_seq + 1);
        }
        std::string clear(data.begin(), data.end());
        if (!clear.empty() && clear.back() == '\0')
        {
            clear.pop_back();
        }
        auto decrypted = decrypted_matches(data);
        if (!decrypted)
        {
            return std::unexpected(decrypted.error());
        }
        if (password_matches(clear) || *decrypted)
        {
            return accept();
        }
        return reject();
    }
    if (plugin == kCachingSha2)
    {
        if (login_password_.empty())
        {
            return (data.empty() || (data.size() == 1 && data[0] == 0)) ? accept() : reject();
        }
        auto expect = caching_sha2_token(login_password_, scramble_);
        if (!expect)
        {
            return std::unexpected(expect.error());
        }
        if (!auth_.require_full_auth && same_bytes(*expect, data))
        {
            auto sent = send_bytes(seq, {0x01, 0x03});
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return accept();
        }
        auto sent = send_bytes(seq, {0x01, 0x04});
        if (!sent)
        {
            return std::unexpected(sent.error());
        }
        uint8_t client_seq = 0;
        auto payload = read_payload(client_seq);
        if (!payload)
        {
            return std::unexpected(payload.error());
        }
        data = std::move(*payload);
        seq = static_cast<uint8_t>(client_seq + 1);
        if (data.size() == 1 && (data[0] == 1 || data[0] == 2))
        {
            std::vector<uint8_t> payload{0x01};
            payload.insert(payload.end(), auth_keys_.public_pem().begin(), auth_keys_.public_pem().end());
            payload.push_back(0);
            sent = send_bytes(seq, payload);
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            auto response = read_payload(client_seq);
            if (!response)
            {
                return std::unexpected(response.error());
            }
            data = std::move(*response);
            seq = static_cast<uint8_t>(client_seq + 1);
        }
        auto decrypted = decrypted_matches(data);
        if (!decrypted)
        {
            return std::unexpected(decrypted.error());
        }
        if (*decrypted)
        {
            return accept();
        }
        return reject();
    }
    return reject();
}

popo::base::Result<void> Session::read_handshake_response()
{
    uint8_t client_seq = 0;
    auto payload = read_payload(client_seq);
    if (!payload)
    {
        return std::unexpected(payload.error());
    }
    auto resp = Protocol::parse_handshake_response(payload->data(), payload->size());
    if (!resp)
    {
        return std::unexpected(resp.error());
    }
    std::string plugin = resp->plugin.empty() ? auth_.plugin : resp->plugin;
    std::vector<uint8_t> data = std::move(resp->auth);
    uint8_t seq = static_cast<uint8_t>(client_seq + 1);
    std::error_code ec;
    auto endpoint = socket_.remote_endpoint(ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to get client endpoint: " + ec.message());
    }
    client_host_ = normalize_client_host(endpoint.address().to_string());
    auto password = accounts_.lookup(resp->user, client_host_, account_host_);
    if (!password)
    {
        auto denied = deny_auth(seq, resp->user);
        if (!denied)
        {
            return denied;
        }
        return popo::base::make_error(popo::base::Errno::AuthDenied, "authentication failed");
    }
    login_password_ = *password;
    current_user_ = resp->user;
    requested_database_ = resp->database;

    if (plugin != kCachingSha2 && plugin != kNativePassword && plugin != kSha256Password && plugin != kClearPassword)
    {
        spdlog::info("Connection {} requested auth plugin {}, switching to {}", connection_id_, plugin, kCachingSha2);
        auto auth_switch = Protocol::build_auth_switch(seq, kCachingSha2, scramble_);
        if (!auth_switch)
        {
            return std::unexpected(auth_switch.error());
        }
        auto sent = send_packet(*auth_switch);
        if (!sent)
        {
            return sent;
        }
        ++seq;
        auto response = read_payload(client_seq);
        if (!response)
        {
            return std::unexpected(response.error());
        }
        data = std::move(*response);
        seq = static_cast<uint8_t>(client_seq + 1);
        plugin = kCachingSha2;
    }

    auto authenticated = finish_auth(plugin, std::move(data), seq, resp->user);
    if (!authenticated)
    {
        return std::unexpected(authenticated.error());
    }
    if (!*authenticated)
    {
        return popo::base::make_error(popo::base::Errno::AuthDenied, "authentication failed");
    }
    return {};
}

popo::base::Result<void> Session::command_loop()
{
    while (socket_.is_open())
    {
        auto read = read_packet();
        if (!read)
        {
            spdlog::debug("Connection {} read error: {}", connection_id_, read.error().error_message);
            return read;
        }
    }
    return {};
}

popo::base::Result<void> Session::read_packet()
{
    auto header_bytes = read_bytes(4);
    if (!header_bytes)
    {
        return std::unexpected(header_bytes.error());
    }
    auto header = Protocol::read_packet_header(header_bytes->data(), header_bytes->size());
    if (!header)
    {
        return std::unexpected(header.error());
    }
    auto payload = read_bytes(header->payload_length);
    if (!payload)
    {
        return std::unexpected(payload.error());
    }
    seq_id_ = 1;
    return handle_command(payload->data(), payload->size());
}

popo::base::Result<void> Session::handle_command(const uint8_t * payload, size_t length)
{
    auto cmd = Protocol::read_command_type(payload, length);
    if (!cmd)
    {
        auto sent = send_error(1064, cmd.error().error_message);
        return sent ? popo::base::Result<void>{} : std::unexpected(sent.error());
    }
    switch (*cmd)
    {
        case CommandType::COM_QUERY: {
            auto query = Protocol::read_query_string(payload, length);
            if (!query)
            {
                auto sent = send_error(1064, query.error().error_message);
                return sent ? popo::base::Result<void>{} : std::unexpected(sent.error());
            }
            return handle_query(*query);
        }
        case CommandType::COM_PING:
            return handle_ping();
        case CommandType::COM_INIT_DB: {
            if (length < 1)
            {
                auto sent = send_error(1064, "MySQL INIT_DB payload is empty");
                return sent ? popo::base::Result<void>{} : std::unexpected(sent.error());
            }
            std::string db_name(reinterpret_cast<const char *>(payload + 1), length - 1);
            if (!db_name.empty() && db_name.back() == 0x00)
                db_name.pop_back();
            return handle_init_db(db_name);
        }
        case CommandType::COM_QUIT: {
            std::error_code ec;
            socket_.close(ec);
            if (ec)
            {
                return popo::base::make_error(
                    popo::base::Errno::IoErr, "failed to close client socket: " + ec.message());
            }
            return {};
        }
        default:
            return send_ok();
    }
}

namespace
{
bool is_ddl(popo::parser::StmtType type)
{
    using popo::parser::StmtType;
    switch (type)
    {
        case StmtType::CreateTable:
        case StmtType::CreateIndex:
        case StmtType::DropTable:
        case StmtType::AlterTable:
        case StmtType::CreateView:
        case StmtType::DropView:
            return true;
        default:
            return false;
    }
}

bool autocommit_value(const std::string & text, bool & out)
{
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "1" || lower == "on" || lower == "true")
    {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "off" || lower == "false")
    {
        out = false;
        return true;
    }
    return false;
}
} // namespace

popo::base::Result<bool> Session::handle_transaction_statement(const popo::parser::Statement & stmt)
{
    using popo::parser::StmtType;
    switch (stmt.type)
    {
        case StmtType::Begin:
        case StmtType::Commit:
        case StmtType::Rollback:
        case StmtType::SetTransaction:
            break;
        default:
            return false;
    }

    // ER_CANT_CHANGE_TX_CHARACTERISTICS for SET TRANSACTION, ER_LOCK_DEADLOCK for
    // a commit that lost a first-committer-wins race.
    const uint16_t code = stmt.type == StmtType::SetTransaction ? 1568 : 1213;
    popo::base::Result<void> result;
    switch (stmt.type)
    {
        case StmtType::Begin:
            result = txn_session_.begin(static_cast<const popo::parser::BeginStatement &>(stmt).isolation);
            break;
        case StmtType::Commit:
            result = txn_session_.commit();
            break;
        case StmtType::Rollback:
            txn_session_.rollback();
            break;
        case StmtType::SetTransaction: {
            const auto & set = static_cast<const popo::parser::SetTransactionStatement &>(stmt);
            result = apply_isolation(set.isolation, set.scope);
            break;
        }
        default:
            break;
    }
    if (!result)
    {
        auto sent = send_error(code, result.error().error_message);
        if (!sent)
        {
            return std::unexpected(sent.error());
        }
        return true;
    }
    auto sent = send_ok();
    if (!sent)
    {
        return std::unexpected(sent.error());
    }
    return true;
}

popo::base::Result<void> Session::apply_isolation(popo::parser::IsolationLevel level, popo::parser::TxnScope scope)
{
    using popo::parser::TxnScope;
    if (scope == TxnScope::Global)
    {
        // Global variable changes are an admin operation; CREATE USER is the
        // global privilege this server already hands to administrators.
        if (!accounts_.has_priv(current_user_, account_host_, popo::parser::kPrivCreateUser, "*", "*"))
        {
            return popo::base::make_error(
                popo::base::Errno::AuthDenied, "Access denied; you need global privileges for SET GLOBAL");
        }
        engine_.set_default_isolation(popo::bytecode::to_storage_isolation(level));
        return {};
    }
    if (scope == TxnScope::Next)
    {
        txn_session_.set_next_isolation(level);
        return {};
    }
    return txn_session_.set_session_isolation(level);
}

popo::base::Result<bool> Session::handle_set_var(const popo::parser::SetVarStatement & stmt)
{
    if (stmt.var_name == "autocommit")
    {
        if (stmt.scope == popo::parser::TxnScope::Global)
        {
            auto sent
                = send_error(1228, "Variable 'autocommit' is a SESSION variable and can't be used with SET GLOBAL");
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return true;
        }
        bool on = true;
        if (!autocommit_value(stmt.value, on))
        {
            auto sent = send_error(1231, "Variable 'autocommit' can't be set to the value of '" + stmt.value + "'");
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return true;
        }
        auto changed = txn_session_.set_autocommit(on);
        if (!changed)
        {
            auto sent = send_error(1213, changed.error().error_message);
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return true;
        }
        auto sent = send_ok();
        if (!sent)
        {
            return std::unexpected(sent.error());
        }
        return true;
    }
    if (stmt.var_name == "tx_isolation" || stmt.var_name == "transaction_isolation")
    {
        auto level = popo::parser::isolation_level_from_name(stmt.value);
        if (!level)
        {
            auto sent = send_error(
                1231, "Variable '" + stmt.var_name + "' can't be set to the value of '" + stmt.value + "'");
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return true;
        }
        auto applied = apply_isolation(*level, stmt.scope);
        if (!applied)
        {
            auto sent
                = send_error(stmt.scope == popo::parser::TxnScope::Global ? 1228 : 1568, applied.error().error_message);
            if (!sent)
            {
                return std::unexpected(sent.error());
            }
            return true;
        }
        auto sent = send_ok();
        if (!sent)
        {
            return std::unexpected(sent.error());
        }
        return true;
    }
    // Other session variables stay inert no-ops so MySQL clients can connect.
    return false;
}

popo::base::Result<void> Session::handle_query(const std::string & sql)
{
    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();

    bool broad_select = accounts_.has_priv(current_user_, account_host_, popo::parser::kPrivSelect, current_db_, "*");
    if (broad_select)
    {
        if (auto fast = popo::bytecode::try_point_select(
                schema_mgr_, engine_, sql, txn_session_.in_transaction() ? txn_session_.current() : nullptr))
        {
            if (!fast->columns.empty())
            {
                return send_result_set(*fast);
            }
            return send_ok(0);
        }
    }

    popo::parser::Parser parser;
    auto parse_result = parser.parse(sql);
    auto t1 = clock::now();
    if (!parse_result)
    {
        return send_error(1064, parse_result.error().error_message);
    }
    auto & stmts = *parse_result;

    popo::planner::Planner planner(schema_mgr_);
    popo::bytecode::Compiler compiler(schema_mgr_);
    popo::bytecode::VirtualMachine vm(schema_mgr_, engine_);
    vm.set_session(&txn_session_);

    for (const auto & stmt : stmts)
    {
        auto transaction = handle_transaction_statement(*stmt);
        if (!transaction)
        {
            return std::unexpected(transaction.error());
        }
        if (*transaction)
        {
            continue;
        }
        if (stmt->type == popo::parser::StmtType::SetVar)
        {
            auto handled = handle_set_var(static_cast<const popo::parser::SetVarStatement &>(*stmt));
            if (!handled)
            {
                return std::unexpected(handled.error());
            }
            if (*handled)
            {
                continue;
            }
        }
        AccountActor actor{current_user_, account_host_, client_host_, current_db_};
        if (is_account_statement(stmt->type))
        {
            auto reply = accounts_.execute(*stmt, actor);
            if (!reply.ok)
            {
                return send_error(reply.code, reply.message);
            }
            if (!reply.column.empty())
            {
                popo::bytecode::ExecuteSuccess result;
                result.columns.push_back(reply.column);
                for (const auto & line : reply.rows)
                {
                    auto row = std::make_shared<popo::type::Row>();
                    row->append_value(popo::type::Value(popo::type::ValueType::String, line));
                    result.rows.push_back(std::move(row));
                }
                auto sent = send_result_set(result);
                if (!sent)
                {
                    return sent;
                }
            }
            else
            {
                auto sent = send_ok();
                if (!sent)
                {
                    return sent;
                }
            }
            continue;
        }
        auto gate = accounts_.authorize(*stmt, actor);
        if (!gate.ok)
        {
            return send_error(gate.code, gate.message);
        }
        // DDL publishes immediately and cannot be rolled back, so it must not sit
        // inside an open transaction (MySQL implicitly commits before DDL).
        if (is_ddl(stmt->type) && txn_session_.in_transaction())
        {
            auto committed = txn_session_.commit();
            if (!committed)
            {
                return send_error(1213, committed.error().error_message);
            }
        }
        auto t_plan0 = clock::now();
        auto plan_result = planner.plan(stmt);
        auto t_plan1 = clock::now();
        if (!plan_result)
        {
            return send_error(1064, plan_result.error().error_message);
        }
        auto & plan = *plan_result;

        auto statement_txn = txn_session_.statement_txn();
        if (!statement_txn)
        {
            return send_error(1213, statement_txn.error().error_message);
        }
        auto * txn = *statement_txn;
        // A failed statement must not leave partial buffered writes behind for
        // the next one, so roll back unless execution reaches the end.
        struct StatementGuard
        {
            popo::bytecode::TxnSession & session;
            bool succeeded = false;
            ~StatementGuard()
            {
                if (!succeeded)
                {
                    session.statement_failed();
                }
            }
        } guard{txn_session_};

        popo::bytecode::ExecuteSuccess exec_result;
        if (popo::executor::needs_generic_executor(stmt) || popo::executor::plan_needs_generic_executor(plan))
        {
            popo::executor::Executor executor(schema_mgr_, engine_);
            auto exec = executor.execute(plan, txn);
            if (!exec)
            {
                return send_error(1064, exec.error().error_message);
            }
            exec_result = std::move(*exec);
        }
        else
        {
            auto comp_result = compiler.compile(plan);
            if (!comp_result)
            {
                return send_error(1064, comp_result.error().error_message);
            }
            auto vm_result = vm.execute(*comp_result, txn);
            if (!vm_result)
            {
                return send_error(1064, vm_result.error().error_message);
            }
            exec_result = std::move(*vm_result);
        }
        guard.succeeded = true;
        auto succeeded = txn_session_.statement_succeeded();
        if (!succeeded)
        {
            return send_error(1213, succeeded.error().error_message);
        }
        auto t_comp1 = clock::now();
        auto t_vm1 = t_comp1;
        auto & result = exec_result;

        auto t_send0 = clock::now();
        if (!result.columns.empty())
        {
            auto sent = send_result_set(result);
            if (!sent)
            {
                return sent;
            }
        }
        else
        {
            auto sent = send_ok(static_cast<uint64_t>(result.rows_affected));
            if (!sent)
            {
                return sent;
            }
        }
        auto t_send1 = clock::now();

        // Performance profiling log
        auto parse_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        auto plan_us = std::chrono::duration_cast<std::chrono::microseconds>(t_plan1 - t_plan0).count();
        auto comp_us = std::chrono::duration_cast<std::chrono::microseconds>(t_comp1 - t_plan1).count();
        auto vm_us = std::chrono::duration_cast<std::chrono::microseconds>(t_vm1 - t_comp1).count();
        auto send_us = std::chrono::duration_cast<std::chrono::microseconds>(t_send1 - t_send0).count();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_send1 - t0).count();

        spdlog::info(
            "[PROFILE] parse={}us plan={}us compile={}us vm={}us send={}us total={}us rows={} cols={} | {}",
            parse_us,
            plan_us,
            comp_us,
            vm_us,
            send_us,
            total_us,
            result.rows_affected,
            result.rows.size(),
            sql.size() > 80 ? sql.substr(0, 80) : sql);
    }
    return {};
}

popo::base::Result<void> Session::handle_ping()
{
    return send_ok();
}

popo::base::Result<void> Session::handle_init_db(const std::string & db_name)
{
    if (!accounts_.can_use_database(current_user_, account_host_, db_name))
    {
        return send_error(
            1044,
            "Access denied for user '" + current_user_ + "'@'" + client_host_ + "' to database '" + db_name + "'");
    }
    current_db_ = db_name;
    spdlog::info("Connection {} init db: {}", connection_id_, db_name);
    return send_ok();
}

popo::base::Result<void> Session::send_packet(const std::vector<uint8_t> & packet)
{
    std::error_code ec;
    const size_t written = asio::write(socket_, asio::buffer(packet), ec);
    if (ec)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "failed to write client packet: " + ec.message());
    }
    if (written != packet.size())
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "short write while sending client packet");
    }
    return {};
}

popo::base::Result<void> Session::send_ok(uint64_t affected_rows, const std::string & msg)
{
    auto packet = Protocol::build_ok_packet(seq_id_++, affected_rows, 0, msg);
    if (!packet)
    {
        return std::unexpected(packet.error());
    }
    return send_packet(*packet);
}

popo::base::Result<void> Session::send_error(uint16_t code, const std::string & msg)
{
    auto packet = Protocol::build_err_packet(seq_id_++, code, msg);
    if (!packet)
    {
        return std::unexpected(packet.error());
    }
    return send_packet(*packet);
}

popo::base::Result<void> Session::send_result_set(const popo::bytecode::ExecuteSuccess & result)
{
    uint8_t seq = seq_id_++;
    auto column_count = Protocol::build_column_count_packet(seq++, result.columns.size());
    if (!column_count)
    {
        return std::unexpected(column_count.error());
    }
    auto sent = send_packet(*column_count);
    if (!sent)
    {
        return sent;
    }
    for (size_t i = 0; i < result.columns.size(); ++i)
    {
        MySQLColumnType col_type = MySQLColumnType::MYSQL_TYPE_VARCHAR;
        uint32_t col_length = 255;
        if (!result.rows.empty() && i < result.rows[0]->len())
        {
            col_type = value_type_to_mysql(result.rows[0]->value_ref(static_cast<uint32_t>(i)).datatype());
            col_length = column_length_for_type(result.rows[0]->value_ref(static_cast<uint32_t>(i)).datatype());
        }
        auto column = Protocol::build_column_def_packet(seq++, "popo", "", result.columns[i], col_type, col_length);
        if (!column)
        {
            return std::unexpected(column.error());
        }
        sent = send_packet(*column);
        if (!sent)
        {
            return sent;
        }
    }
    auto eof = Protocol::build_eof_packet(seq++);
    if (!eof)
    {
        return std::unexpected(eof.error());
    }
    sent = send_packet(*eof);
    if (!sent)
    {
        return sent;
    }
    for (const auto & row : result.rows)
    {
        std::vector<uint8_t> payload;
        for (uint32_t i = 0; i < row->len(); ++i)
        {
            const auto & val = row->value_ref(i);
            if (val.datatype() == popo::type::ValueType::Null)
            {
                payload.push_back(0xFB);
            }
            else
            {
                Protocol::write_lenenc_string(payload, val.to_val_string());
            }
        }
        auto packet = Protocol::make_packet(seq++, payload);
        if (!packet)
        {
            return std::unexpected(packet.error());
        }
        sent = send_packet(*packet);
        if (!sent)
        {
            return sent;
        }
    }
    eof = Protocol::build_eof_packet(seq++);
    if (!eof)
    {
        return std::unexpected(eof.error());
    }
    return send_packet(*eof);
}

popo::base::Result<std::vector<uint8_t>> Session::read_bytes(size_t n)
{
    std::vector<uint8_t> buf(n);
    std::error_code ec;
    const size_t read = asio::read(socket_, asio::buffer(buf), ec);
    if (ec)
    {
        const auto code = ec == asio::error::eof ? popo::base::Errno::EndOfStream : popo::base::Errno::IoErr;
        return popo::base::make_error(code, "failed to read client packet: " + ec.message());
    }
    if (read != n)
    {
        return popo::base::make_error(popo::base::Errno::EndOfStream, "short read while receiving client packet");
    }
    return buf;
}

MySQLColumnType Session::value_type_to_mysql(popo::type::ValueType vt)
{
    switch (vt)
    {
        case popo::type::ValueType::Int8:
        case popo::type::ValueType::Uint8:
            return MySQLColumnType::MYSQL_TYPE_TINY;
        case popo::type::ValueType::Int16:
        case popo::type::ValueType::Uint16:
            return MySQLColumnType::MYSQL_TYPE_SHORT;
        case popo::type::ValueType::Int32:
        case popo::type::ValueType::Uint32:
            return MySQLColumnType::MYSQL_TYPE_LONG;
        case popo::type::ValueType::Int64:
        case popo::type::ValueType::Uint64:
            return MySQLColumnType::MYSQL_TYPE_LONGLONG;
        case popo::type::ValueType::Float32:
            return MySQLColumnType::MYSQL_TYPE_FLOAT;
        case popo::type::ValueType::Float64:
            return MySQLColumnType::MYSQL_TYPE_DOUBLE;
        case popo::type::ValueType::String:
            return MySQLColumnType::MYSQL_TYPE_VARCHAR;
        case popo::type::ValueType::Char:
            return MySQLColumnType::MYSQL_TYPE_STRING;
        case popo::type::ValueType::Bool:
            return MySQLColumnType::MYSQL_TYPE_TINY;
        case popo::type::ValueType::Null:
            return MySQLColumnType::MYSQL_TYPE_NULL;
        default:
            return MySQLColumnType::MYSQL_TYPE_VARCHAR;
    }
}

std::string Session::value_to_string(const popo::type::Value & val)
{
    return val.to_val_string();
}

uint32_t Session::column_length_for_type(popo::type::ValueType vt)
{
    switch (vt)
    {
        case popo::type::ValueType::Int8:
            return 4;
        case popo::type::ValueType::Uint8:
            return 3;
        case popo::type::ValueType::Int16:
            return 6;
        case popo::type::ValueType::Uint16:
            return 5;
        case popo::type::ValueType::Int32:
            return 11;
        case popo::type::ValueType::Uint32:
            return 10;
        case popo::type::ValueType::Int64:
            return 20;
        case popo::type::ValueType::Uint64:
            return 20;
        case popo::type::ValueType::Float32:
            return 12;
        case popo::type::ValueType::Float64:
            return 22;
        case popo::type::ValueType::Bool:
            return 1;
        case popo::type::ValueType::Char:
            return 1;
        case popo::type::ValueType::String:
            return 255;
        case popo::type::ValueType::Null:
            return 0;
        default:
            return 255;
    }
}

} // namespace popo::server
