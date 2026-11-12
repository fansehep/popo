#include "server/protocol.h"
#include "server/auth.h"

namespace popo::server
{

namespace
{

std::unexpected<popo::base::Error> protocol_error(std::string message)
{
    return popo::base::make_error(popo::base::Errno::ProtocolErr, std::move(message));
}

} // namespace

popo::base::Result<std::vector<uint8_t>> Protocol::make_packet(uint8_t seq_id, const std::vector<uint8_t> & payload)
{
    if (payload.size() > 0xFFFFFF)
    {
        return protocol_error("MySQL packet payload exceeds 16 MiB framing limit");
    }
    std::vector<uint8_t> packet;
    uint32_t len = static_cast<uint32_t>(payload.size());
    packet.push_back(len & 0xFF);
    packet.push_back((len >> 8) & 0xFF);
    packet.push_back((len >> 16) & 0xFF);
    packet.push_back(seq_id);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

void Protocol::write_lenenc_int(std::vector<uint8_t> & buf, uint64_t val)
{
    if (val < 251)
    {
        buf.push_back(static_cast<uint8_t>(val));
    }
    else if (val <= 0xFFFF)
    {
        buf.push_back(0xFC);
        buf.push_back(val & 0xFF);
        buf.push_back((val >> 8) & 0xFF);
    }
    else if (val <= 0xFFFFFF)
    {
        buf.push_back(0xFD);
        buf.push_back(val & 0xFF);
        buf.push_back((val >> 8) & 0xFF);
        buf.push_back((val >> 16) & 0xFF);
    }
    else
    {
        buf.push_back(0xFE);
        for (int i = 0; i < 8; ++i)
        {
            buf.push_back((val >> (8 * i)) & 0xFF);
        }
    }
}

popo::base::Result<uint64_t> Protocol::read_lenenc_int(const uint8_t *& data, size_t & remaining)
{
    if (data == nullptr || remaining == 0)
        return protocol_error("length-encoded integer is missing");
    uint8_t first = *data;
    if (first < 0xFB)
    {
        data++;
        remaining--;
        return first;
    }
    else if (first == 0xFC)
    {
        if (remaining < 3)
            return protocol_error("2-byte length-encoded integer is truncated");
        uint64_t val = data[1] | (static_cast<uint64_t>(data[2]) << 8);
        data += 3;
        remaining -= 3;
        return val;
    }
    else if (first == 0xFD)
    {
        if (remaining < 4)
            return protocol_error("3-byte length-encoded integer is truncated");
        uint64_t val = data[1] | (static_cast<uint64_t>(data[2]) << 8) | (static_cast<uint64_t>(data[3]) << 16);
        data += 4;
        remaining -= 4;
        return val;
    }
    else if (first == 0xFE)
    {
        if (remaining < 9)
            return protocol_error("8-byte length-encoded integer is truncated");
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i)
        {
            val |= static_cast<uint64_t>(data[1 + i]) << (8 * i);
        }
        data += 9;
        remaining -= 9;
        return val;
    }
    if (first == 0xFB)
        return protocol_error("NULL length-encoded integer is not supported here");
    return protocol_error("invalid length-encoded integer prefix");
}

void Protocol::write_lenenc_string(std::vector<uint8_t> & buf, const std::string & str)
{
    write_lenenc_int(buf, str.size());
    buf.insert(buf.end(), str.begin(), str.end());
}

popo::base::Result<std::string> Protocol::read_lenenc_string(const uint8_t *& data, size_t & remaining)
{
    auto len = read_lenenc_int(data, remaining);
    if (!len)
        return std::unexpected(len.error());
    if (*len > remaining)
        return protocol_error("length-encoded string is truncated");
    std::string result(reinterpret_cast<const char *>(data), static_cast<size_t>(*len));
    data += static_cast<size_t>(*len);
    remaining -= static_cast<size_t>(*len);
    return result;
}

void Protocol::write_null_string(std::vector<uint8_t> & buf, const std::string & str)
{
    buf.insert(buf.end(), str.begin(), str.end());
    buf.push_back(0);
}

void Protocol::write_int1(std::vector<uint8_t> & buf, uint8_t val)
{
    buf.push_back(val);
}

void Protocol::write_int2(std::vector<uint8_t> & buf, uint16_t val)
{
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
}

void Protocol::write_int3(std::vector<uint8_t> & buf, uint32_t val)
{
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
}

void Protocol::write_int4(std::vector<uint8_t> & buf, uint32_t val)
{
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}
popo::base::Result<std::vector<uint8_t>> Protocol::build_handshake_v10(
    uint32_t connection_id, const std::vector<uint8_t> & scramble, const std::string & auth_plugin)
{
    if (scramble.size() < 20)
    {
        return protocol_error("auth scramble must be at least 20 bytes");
    }
    std::vector<uint8_t> payload;
    payload.push_back(10);
    write_null_string(payload, kServerVersion);
    write_int4(payload, connection_id);
    payload.insert(payload.end(), scramble.begin(), scramble.begin() + 8);
    payload.push_back(0x00);
    write_int2(payload, static_cast<uint16_t>(kServerCapabilities & 0xFFFF));
    payload.push_back(45); // utf8mb4_general_ci
    write_int2(payload, 0x0002); // SERVER_STATUS_AUTOCOMMIT
    write_int2(payload, static_cast<uint16_t>(kServerCapabilities >> 16));
    payload.push_back(21); // 20-byte scramble plus a NUL
    for (int i = 0; i < 10; ++i)
    {
        payload.push_back(0);
    }
    payload.insert(payload.end(), scramble.begin() + 8, scramble.begin() + 20);
    payload.push_back(0);
    write_null_string(payload, auth_plugin);
    return make_packet(0, payload);
}

popo::base::Result<std::vector<uint8_t>>
Protocol::build_auth_switch(uint8_t seq_id, const std::string & plugin, const std::vector<uint8_t> & scramble)
{
    std::vector<uint8_t> payload;
    payload.push_back(0xFE);
    write_null_string(payload, plugin);
    payload.insert(payload.end(), scramble.begin(), scramble.end());
    payload.push_back(0);
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>> Protocol::build_auth_more(uint8_t seq_id, const std::vector<uint8_t> & data)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    payload.insert(payload.end(), data.begin(), data.end());
    return make_packet(seq_id, payload);
}

popo::base::Result<HandshakeResponse> Protocol::parse_handshake_response(const uint8_t * payload, size_t length)
{
    if (payload == nullptr || length < 32)
    {
        return protocol_error("handshake response is too short");
    }
    HandshakeResponse resp;
    resp.capabilities = payload[0] | (static_cast<uint32_t>(payload[1]) << 8)
        | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
    const uint8_t * p = payload + 32;
    size_t remaining = length - 32;
    while (remaining > 0 && *p != 0)
    {
        resp.user.push_back(static_cast<char>(*p));
        ++p;
        --remaining;
    }
    if (remaining == 0)
    {
        return protocol_error("handshake response is missing the username terminator");
    }
    ++p;
    --remaining;

    if ((resp.capabilities & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) != 0)
    {
        auto auth_len = read_lenenc_int(p, remaining);
        if (!auth_len)
        {
            return std::unexpected(auth_len.error());
        }
        if (*auth_len > remaining)
        {
            return protocol_error("handshake auth data is truncated");
        }
        resp.auth.assign(p, p + static_cast<size_t>(*auth_len));
        p += static_cast<size_t>(*auth_len);
        remaining -= static_cast<size_t>(*auth_len);
    }
    else if ((resp.capabilities & CLIENT_SECURE_CONNECTION) != 0)
    {
        if (remaining == 0)
        {
            return protocol_error("handshake auth length is missing");
        }
        uint8_t auth_len = *p;
        ++p;
        --remaining;
        if (auth_len > remaining)
        {
            return protocol_error("handshake auth data is truncated");
        }
        resp.auth.assign(p, p + auth_len);
        p += auth_len;
        remaining -= auth_len;
    }
    else
    {
        while (remaining > 0 && *p != 0)
        {
            resp.auth.push_back(*p);
            ++p;
            --remaining;
        }
        if (remaining > 0)
        {
            ++p;
            --remaining;
        }
    }

    if ((resp.capabilities & CLIENT_CONNECT_WITH_DB) != 0 && remaining > 0)
    {
        while (remaining > 0 && *p != 0)
        {
            resp.database.push_back(static_cast<char>(*p));
            ++p;
            --remaining;
        }
        if (remaining > 0)
        {
            ++p;
            --remaining;
        }
    }
    if ((resp.capabilities & CLIENT_PLUGIN_AUTH) != 0 && remaining > 0)
    {
        while (remaining > 0 && *p != 0)
        {
            resp.plugin.push_back(static_cast<char>(*p));
            ++p;
            --remaining;
        }
    }
    return resp;
}

popo::base::Result<std::vector<uint8_t>>
Protocol::build_ok_packet(uint8_t seq_id, uint64_t affected_rows, uint64_t last_insert_id, const std::string & message)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x00);
    write_lenenc_int(payload, affected_rows);
    write_lenenc_int(payload, last_insert_id);
    write_int2(payload, 0x0002);
    write_int2(payload, 0);
    if (!message.empty())
    {
        write_lenenc_string(payload, message);
    }
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>>
Protocol::build_err_packet(uint8_t seq_id, uint16_t error_code, const std::string & message)
{
    std::vector<uint8_t> payload;
    payload.push_back(0xFF);
    write_int2(payload, error_code);
    payload.push_back('#');
    payload.insert(payload.end(), {'H', 'Y', '0', '0', '0'});
    payload.insert(payload.end(), message.begin(), message.end());
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>> Protocol::build_eof_packet(uint8_t seq_id)
{
    std::vector<uint8_t> payload;
    payload.push_back(0xFE);
    write_int2(payload, 0);
    write_int2(payload, 0x0002);
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>> Protocol::build_column_count_packet(uint8_t seq_id, uint64_t count)
{
    std::vector<uint8_t> payload;
    write_lenenc_int(payload, count);
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>> Protocol::build_column_def_packet(
    uint8_t seq_id,
    const std::string & schema,
    const std::string & table,
    const std::string & column,
    MySQLColumnType col_type,
    uint32_t col_length)
{
    std::vector<uint8_t> payload;
    write_lenenc_string(payload, "def");
    write_lenenc_string(payload, schema);
    write_lenenc_string(payload, table);
    write_lenenc_string(payload, table);
    write_lenenc_string(payload, column);
    write_lenenc_string(payload, column);
    payload.push_back(0x0C);
    write_int2(payload, 0x21);
    write_int4(payload, col_length);
    payload.push_back(static_cast<uint8_t>(col_type));
    write_int2(payload, 0);
    payload.push_back(0);
    write_int2(payload, 0);
    return make_packet(seq_id, payload);
}

popo::base::Result<std::vector<uint8_t>>
Protocol::build_text_row_packet(uint8_t seq_id, const std::vector<std::string> & values)
{
    std::vector<uint8_t> payload;
    for (const auto & val : values)
    {
        write_lenenc_string(payload, val);
    }
    return make_packet(seq_id, payload);
}

popo::base::Result<PacketHeader> Protocol::read_packet_header(const uint8_t * data, size_t length)
{
    if (data == nullptr || length < 4)
    {
        return protocol_error("MySQL packet header is truncated");
    }
    PacketHeader header;
    header.payload_length = data[0] | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16);
    header.sequence_id = data[3];
    return header;
}

popo::base::Result<CommandType> Protocol::read_command_type(const uint8_t * payload, size_t length)
{
    if (payload == nullptr || length == 0)
    {
        return protocol_error("MySQL command payload is empty");
    }
    return static_cast<CommandType>(*payload);
}

popo::base::Result<std::string> Protocol::read_query_string(const uint8_t * payload, size_t length)
{
    if (payload == nullptr || length == 0)
        return protocol_error("MySQL query payload is empty");
    if (length == 1)
        return "";
    return std::string(reinterpret_cast<const char *>(payload + 1), length - 1);
}

} // namespace popo::server
