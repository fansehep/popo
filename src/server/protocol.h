#pragma once

#include "base/error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace popo::server
{

struct PacketHeader
{
    uint32_t payload_length;
    uint8_t sequence_id;
};

enum CapabilityFlags : uint32_t
{
    CLIENT_LONG_PASSWORD = 1,
    CLIENT_FOUND_ROWS = 2,
    CLIENT_LONG_FLAG = 4,
    CLIENT_CONNECT_WITH_DB = 8,
    CLIENT_PROTOCOL_41 = 0x00000200,
    CLIENT_TRANSACTIONS = 0x00002000,
    CLIENT_SECURE_CONNECTION = 0x00008000,
    CLIENT_MULTI_STATEMENTS = 0x00010000,
    CLIENT_MULTI_RESULTS = 0x00020000,
    CLIENT_PS_MULTI_RESULTS = 0x00040000,
    CLIENT_PLUGIN_AUTH = 0x00080000,
    CLIENT_CONNECT_ATTRS = 0x00100000,
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 0x00200000,
    CLIENT_DEPRECATE_EOF = 0x01000000,
};

inline constexpr uint32_t kServerCapabilities = CLIENT_LONG_PASSWORD | CLIENT_FOUND_ROWS | CLIENT_LONG_FLAG
    | CLIENT_CONNECT_WITH_DB | CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION
    | CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS | CLIENT_PS_MULTI_RESULTS | CLIENT_PLUGIN_AUTH
    | CLIENT_CONNECT_ATTRS | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA;

struct HandshakeResponse
{
    uint32_t capabilities = 0;
    std::string user;
    std::string database;
    std::string plugin;
    std::vector<uint8_t> auth;
};

enum class CommandType : uint8_t
{
    COM_QUIT = 0x01,
    COM_INIT_DB = 0x02,
    COM_QUERY = 0x03,
    COM_PING = 0x0E,
};

enum class MySQLColumnType : uint8_t
{
    MYSQL_TYPE_LONG = 0x03,
    MYSQL_TYPE_LONGLONG = 0x08,
    MYSQL_TYPE_TINY = 0x01,
    MYSQL_TYPE_SHORT = 0x02,
    MYSQL_TYPE_FLOAT = 0x04,
    MYSQL_TYPE_DOUBLE = 0x05,
    MYSQL_TYPE_VARCHAR = 0x0F,
    MYSQL_TYPE_STRING = 0xFE,
    MYSQL_TYPE_NULL = 0x06,
    MYSQL_TYPE_TINY_BLOB = 0x01,
};

class Protocol
{
public:
    static popo::base::Result<std::vector<uint8_t>> make_packet(uint8_t seq_id, const std::vector<uint8_t> & payload);
    static void write_lenenc_int(std::vector<uint8_t> & buf, uint64_t val);
    static popo::base::Result<uint64_t> read_lenenc_int(const uint8_t *& data, size_t & remaining);
    static void write_lenenc_string(std::vector<uint8_t> & buf, const std::string & str);
    static popo::base::Result<std::string> read_lenenc_string(const uint8_t *& data, size_t & remaining);
    static void write_null_string(std::vector<uint8_t> & buf, const std::string & str);
    static void write_int1(std::vector<uint8_t> & buf, uint8_t val);
    static void write_int2(std::vector<uint8_t> & buf, uint16_t val);
    static void write_int3(std::vector<uint8_t> & buf, uint32_t val);
    static void write_int4(std::vector<uint8_t> & buf, uint32_t val);
    static popo::base::Result<std::vector<uint8_t>>
    build_handshake_v10(uint32_t connection_id, const std::vector<uint8_t> & scramble, const std::string & auth_plugin);
    static popo::base::Result<std::vector<uint8_t>>
    build_auth_switch(uint8_t seq_id, const std::string & plugin, const std::vector<uint8_t> & scramble);
    static popo::base::Result<std::vector<uint8_t>> build_auth_more(uint8_t seq_id, const std::vector<uint8_t> & data);
    static popo::base::Result<HandshakeResponse> parse_handshake_response(const uint8_t * payload, size_t length);
    static popo::base::Result<std::vector<uint8_t>> build_ok_packet(
        uint8_t seq_id, uint64_t affected_rows = 0, uint64_t last_insert_id = 0, const std::string & message = "");
    static popo::base::Result<std::vector<uint8_t>>
    build_err_packet(uint8_t seq_id, uint16_t error_code, const std::string & message);
    static popo::base::Result<std::vector<uint8_t>> build_eof_packet(uint8_t seq_id);
    static popo::base::Result<std::vector<uint8_t>> build_column_count_packet(uint8_t seq_id, uint64_t count);
    static popo::base::Result<std::vector<uint8_t>> build_column_def_packet(
        uint8_t seq_id,
        const std::string & schema,
        const std::string & table,
        const std::string & column,
        MySQLColumnType col_type,
        uint32_t col_length);
    static popo::base::Result<std::vector<uint8_t>>
    build_text_row_packet(uint8_t seq_id, const std::vector<std::string> & values);
    static popo::base::Result<PacketHeader> read_packet_header(const uint8_t * data, size_t length);
    static popo::base::Result<CommandType> read_command_type(const uint8_t * payload, size_t length);
    static popo::base::Result<std::string> read_query_string(const uint8_t * payload, size_t length);
};

} // namespace popo::server
