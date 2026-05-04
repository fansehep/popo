#include "server/session.h"
#include "server/protocol.h"
#include <spdlog/spdlog.h>
#include <cctype>

namespace neon::server {

namespace {
    std::string to_lower(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return result;
    }
}

Session::Session(asio::ip::tcp::socket socket, neon::catalog::SchemaManager& schema_mgr, neon::storage::StorageEngine& engine, uint32_t connection_id)
    : socket_(std::move(socket))
    , schema_mgr_(schema_mgr)
    , engine_(engine)
    , connection_id_(connection_id) {}

void Session::start() {
    try {
        do_handshake();
        read_handshake_response();
        command_loop();
    } catch (const std::exception& e) {
        spdlog::warn("Session {} closed: {}", connection_id_, e.what());
    }
}

void Session::do_handshake() {
    auto packet = Protocol::build_handshake_v10(connection_id_);
    send_packet(packet);
}

void Session::read_handshake_response() {
    auto header_bytes = read_bytes(4);
    auto header = Protocol::read_packet_header(header_bytes.data());
    auto payload = read_bytes(header.payload_length);
    seq_id_ = 2;
    send_ok();
}

void Session::command_loop() {
    while (socket_.is_open()) {
        try {
            read_packet();
        } catch (const std::exception& e) {
            spdlog::debug("Connection {} read error: {}", connection_id_, e.what());
            break;
        }
    }
}

void Session::read_packet() {
    auto header_bytes = read_bytes(4);
    auto header = Protocol::read_packet_header(header_bytes.data());
    auto payload = read_bytes(header.payload_length);
    seq_id_ = 1;
    handle_command(payload.data(), header.payload_length);
}

void Session::handle_command(const uint8_t* payload, size_t length) {
    auto cmd = Protocol::read_command_type(payload);
    switch (cmd) {
        case CommandType::COM_QUERY:
            handle_query(Protocol::read_query_string(payload, length));
            break;
        case CommandType::COM_PING:
            handle_ping();
            break;
        case CommandType::COM_INIT_DB: {
            std::string db_name(reinterpret_cast<const char*>(payload + 1), length - 1);
            if (!db_name.empty() && db_name.back() == 0x00) db_name.pop_back();
            handle_init_db(db_name);
            break;
        }
        case CommandType::COM_QUIT:
            socket_.close();
            break;
        default:
            send_ok();
            break;
    }
}

void Session::handle_query(const std::string& sql) {
    std::string lower_sql = to_lower(sql);

    if (lower_sql.find("@@version_comment") != std::string::npos) {
        neon::bytecode::ExecuteResult result;
        result.success = true;
        result.columns = {"@@version_comment"};
        auto row = std::make_shared<neon::type::Row>();
        row->append_value(neon::type::Value(neon::type::ValueType::String, "neon database"));
        result.rows.push_back(row);
        send_result_set(result);
        return;
    }

    if (lower_sql.starts_with("set ")) {
        send_ok();
        return;
    }

    if (lower_sql == "select database()") {
        neon::bytecode::ExecuteResult result;
        result.success = true;
        result.columns = {"DATABASE()"};
        auto row = std::make_shared<neon::type::Row>();
        row->append_value(neon::type::Value(neon::type::ValueType::String, "neon"));
        result.rows.push_back(row);
        send_result_set(result);
        return;
    }

    if (lower_sql == "show databases") {
        neon::bytecode::ExecuteResult result;
        result.success = true;
        result.columns = {"Database"};
        auto row = std::make_shared<neon::type::Row>();
        row->append_value(neon::type::Value(neon::type::ValueType::String, "neon"));
        result.rows.push_back(row);
        send_result_set(result);
        return;
    }

    if (lower_sql == "show tables") {
        neon::bytecode::ExecuteResult result;
        result.success = true;
        result.columns = {"Tables_in_neon"};
        send_result_set(result);
        return;
    }

    if (lower_sql.starts_with("show variables") || lower_sql.starts_with("show status")) {
        neon::bytecode::ExecuteResult result;
        result.success = true;
        result.columns = {"Variable_name", "Value"};
        send_result_set(result);
        return;
    }

    neon::parser::Parser parser;
    auto [stmts, err] = parser.parse(sql);
    if (err.error_code != 0) {
        send_error(1064, err.error_message);
        return;
    }

    neon::planner::Planner planner(schema_mgr_);
    neon::bytecode::Compiler compiler(schema_mgr_);
    neon::bytecode::VirtualMachine vm(schema_mgr_, engine_);

    for (const auto& stmt : stmts) {
        auto [plan, plan_err] = planner.plan(stmt);
        if (plan_err.error_code != 0) {
            send_error(1064, plan_err.error_message);
            return;
        }
        auto [program, comp_err] = compiler.compile(plan);
        if (comp_err.error_code != 0) {
            send_error(1064, comp_err.error_message);
            return;
        }
        auto result = vm.execute(program);
        if (!result.success) {
            send_error(1064, result.error.error_message);
            return;
        }
        if (!result.columns.empty()) {
            send_result_set(result);
        } else {
            send_ok(static_cast<uint64_t>(result.rows_affected));
        }
    }
}

void Session::handle_ping() {
    send_ok();
}

void Session::handle_init_db(const std::string& db_name) {
    spdlog::info("Connection {} init db: {}", connection_id_, db_name);
    send_ok();
}

void Session::send_packet(const std::vector<uint8_t>& packet) {
    asio::write(socket_, asio::buffer(packet));
}

void Session::send_ok(uint64_t affected_rows, const std::string& msg) {
    auto packet = Protocol::build_ok_packet(seq_id_++, affected_rows, 0, msg);
    send_packet(packet);
}

void Session::send_error(uint16_t code, const std::string& msg) {
    auto packet = Protocol::build_err_packet(seq_id_++, code, msg);
    send_packet(packet);
}

void Session::send_result_set(const neon::bytecode::ExecuteResult& result) {
    uint8_t seq = seq_id_++;
    send_packet(Protocol::build_column_count_packet(seq++, result.columns.size()));
    for (size_t i = 0; i < result.columns.size(); ++i) {
        MySQLColumnType col_type = MySQLColumnType::MYSQL_TYPE_VARCHAR;
        uint32_t col_length = 255;
        if (!result.rows.empty() && i < result.rows[0]->len()) {
            col_type = value_type_to_mysql(result.rows[0]->value_ref(static_cast<uint32_t>(i)).datatype());
            col_length = column_length_for_type(result.rows[0]->value_ref(static_cast<uint32_t>(i)).datatype());
        }
        send_packet(Protocol::build_column_def_packet(
            seq++, "neon", "", result.columns[i], col_type, col_length));
    }
    send_packet(Protocol::build_eof_packet(seq++));
    for (const auto& row : result.rows) {
        std::vector<uint8_t> payload;
        for (uint32_t i = 0; i < row->len(); ++i) {
            const auto& val = row->value_ref(i);
            if (val.datatype() == neon::type::ValueType::Null) {
                payload.push_back(0xFB);
            } else {
                Protocol::write_lenenc_string(payload, val.to_val_string());
            }
        }
        send_packet(Protocol::make_packet(seq++, payload));
    }
    send_packet(Protocol::build_eof_packet(seq++));
}

std::vector<uint8_t> Session::read_bytes(size_t n) {
    std::vector<uint8_t> buf(n);
    asio::read(socket_, asio::buffer(buf));
    return buf;
}

MySQLColumnType Session::value_type_to_mysql(neon::type::ValueType vt) {
    switch (vt) {
        case neon::type::ValueType::Int8:
        case neon::type::ValueType::Uint8:
            return MySQLColumnType::MYSQL_TYPE_TINY;
        case neon::type::ValueType::Int16:
        case neon::type::ValueType::Uint16:
            return MySQLColumnType::MYSQL_TYPE_SHORT;
        case neon::type::ValueType::Int32:
        case neon::type::ValueType::Uint32:
            return MySQLColumnType::MYSQL_TYPE_LONG;
        case neon::type::ValueType::Int64:
        case neon::type::ValueType::Uint64:
            return MySQLColumnType::MYSQL_TYPE_LONGLONG;
        case neon::type::ValueType::Float32:
            return MySQLColumnType::MYSQL_TYPE_FLOAT;
        case neon::type::ValueType::Float64:
            return MySQLColumnType::MYSQL_TYPE_DOUBLE;
        case neon::type::ValueType::String:
            return MySQLColumnType::MYSQL_TYPE_VARCHAR;
        case neon::type::ValueType::Char:
            return MySQLColumnType::MYSQL_TYPE_STRING;
        case neon::type::ValueType::Bool:
            return MySQLColumnType::MYSQL_TYPE_TINY;
        case neon::type::ValueType::Null:
            return MySQLColumnType::MYSQL_TYPE_NULL;
        default:
            return MySQLColumnType::MYSQL_TYPE_VARCHAR;
    }
}

std::string Session::value_to_string(const neon::type::Value& val) {
    return val.to_val_string();
}

uint32_t Session::column_length_for_type(neon::type::ValueType vt) {
    switch (vt) {
        case neon::type::ValueType::Int8: return 4;
        case neon::type::ValueType::Uint8: return 3;
        case neon::type::ValueType::Int16: return 6;
        case neon::type::ValueType::Uint16: return 5;
        case neon::type::ValueType::Int32: return 11;
        case neon::type::ValueType::Uint32: return 10;
        case neon::type::ValueType::Int64: return 20;
        case neon::type::ValueType::Uint64: return 20;
        case neon::type::ValueType::Float32: return 12;
        case neon::type::ValueType::Float64: return 22;
        case neon::type::ValueType::Bool: return 1;
        case neon::type::ValueType::Char: return 1;
        case neon::type::ValueType::String: return 255;
        case neon::type::ValueType::Null: return 0;
        default: return 255;
    }
}

} // namespace neon::server
