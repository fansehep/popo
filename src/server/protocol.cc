#include "server/protocol.h"

namespace neon::server
{

std::vector<uint8_t> Protocol::make_packet(uint8_t seq_id, const std::vector<uint8_t> & payload)
{
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

uint64_t Protocol::read_lenenc_int(const uint8_t *& data, size_t & remaining)
{
    if (remaining == 0)
        return 0;
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
            return 0;
        uint64_t val = data[1] | (static_cast<uint64_t>(data[2]) << 8);
        data += 3;
        remaining -= 3;
        return val;
    }
    else if (first == 0xFD)
    {
        if (remaining < 4)
            return 0;
        uint64_t val = data[1] | (static_cast<uint64_t>(data[2]) << 8) | (static_cast<uint64_t>(data[3]) << 16);
        data += 4;
        remaining -= 4;
        return val;
    }
    else if (first == 0xFE)
    {
        if (remaining < 9)
            return 0;
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i)
        {
            val |= static_cast<uint64_t>(data[1 + i]) << (8 * i);
        }
        data += 9;
        remaining -= 9;
        return val;
    }
    return 0;
}

void Protocol::write_lenenc_string(std::vector<uint8_t> & buf, const std::string & str)
{
    write_lenenc_int(buf, str.size());
    buf.insert(buf.end(), str.begin(), str.end());
}

std::string Protocol::read_lenenc_string(const uint8_t *& data, size_t & remaining)
{
    uint64_t len = read_lenenc_int(data, remaining);
    if (len > remaining)
        return "";
    std::string result(reinterpret_cast<const char *>(data), static_cast<size_t>(len));
    data += len;
    remaining -= static_cast<size_t>(len);
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
std::vector<uint8_t> Protocol::build_handshake_v10(uint32_t connection_id)
{
    std::vector<uint8_t> payload;
    payload.push_back(10);
    write_null_string(payload, "neon-database-v0.1");
    write_int4(payload, connection_id);
    for (int i = 0; i < 8; ++i)
        payload.push_back(0);
    payload.push_back(0x00);
    uint16_t cap_lower = static_cast<uint16_t>(CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION);
    write_int2(payload, cap_lower);
    payload.push_back(0x21);
    uint16_t status_flags = 0x0002;
    write_int2(payload, status_flags);
    uint16_t cap_upper = static_cast<uint16_t>((CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH) >> 16);
    write_int2(payload, cap_upper);
    payload.push_back(21);
    for (int i = 0; i < 10; ++i)
        payload.push_back(0);
    for (int i = 0; i < 12; ++i)
        payload.push_back(0);
    payload.push_back(0);
    write_null_string(payload, "mysql_native_password");
    return make_packet(0, payload);
}

std::vector<uint8_t> Protocol::build_ok_packet(uint8_t seq_id, uint64_t affected_rows, uint64_t last_insert_id, const std::string & message)
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

std::vector<uint8_t> Protocol::build_err_packet(uint8_t seq_id, uint16_t error_code, const std::string & message)
{
    std::vector<uint8_t> payload;
    payload.push_back(0xFF);
    write_int2(payload, error_code);
    payload.push_back('#');
    payload.insert(payload.end(), {'H', 'Y', '0', '0', '0'});
    payload.insert(payload.end(), message.begin(), message.end());
    return make_packet(seq_id, payload);
}

std::vector<uint8_t> Protocol::build_eof_packet(uint8_t seq_id)
{
    std::vector<uint8_t> payload;
    payload.push_back(0xFE);
    write_int2(payload, 0);
    write_int2(payload, 0x0002);
    return make_packet(seq_id, payload);
}

std::vector<uint8_t> Protocol::build_column_count_packet(uint8_t seq_id, uint64_t count)
{
    std::vector<uint8_t> payload;
    write_lenenc_int(payload, count);
    return make_packet(seq_id, payload);
}

std::vector<uint8_t> Protocol::build_column_def_packet(
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

std::vector<uint8_t> Protocol::build_text_row_packet(uint8_t seq_id, const std::vector<std::string> & values)
{
    std::vector<uint8_t> payload;
    for (const auto & val : values)
    {
        write_lenenc_string(payload, val);
    }
    return make_packet(seq_id, payload);
}

PacketHeader Protocol::read_packet_header(const uint8_t * data)
{
    PacketHeader header;
    header.payload_length = data[0] | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16);
    header.sequence_id = data[3];
    return header;
}

CommandType Protocol::read_command_type(const uint8_t * payload)
{
    return static_cast<CommandType>(*payload);
}

std::string Protocol::read_query_string(const uint8_t * payload, size_t length)
{
    if (length <= 1)
        return "";
    return std::string(reinterpret_cast<const char *>(payload + 1), length - 1);
}

} // namespace neon::server
