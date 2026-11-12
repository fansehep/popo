#include "src/catalog/schema_io.h"
#include <cstring>
#include "src/type/value.h"

namespace popo::catalog
{

namespace
{

void put_u8(std::vector<char> & b, uint8_t v) { b.push_back(static_cast<char>(v)); }
void put_u32(std::vector<char> & b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_i32(std::vector<char> & b, int32_t v) { put_u32(b, static_cast<uint32_t>(v)); }
void put_u64(std::vector<char> & b, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_str(std::vector<char> & b, const std::string & s)
{
    put_u32(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader
{
    const char * p = nullptr;
    const char * end = nullptr;
    bool ok = true;

    bool need(size_t n)
    {
        if (!ok || p == nullptr || static_cast<size_t>(end - p) < n)
        {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8()
    {
        if (!need(1))
            return 0;
        return static_cast<uint8_t>(*p++);
    }
    uint32_t u32()
    {
        if (!need(4))
            return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(*p++)) << (8 * i);
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64()
    {
        if (!need(8))
            return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(*p++)) << (8 * i);
        return v;
    }
    std::string str()
    {
        uint32_t n = u32();
        if (!need(n))
            return {};
        std::string s(p, p + n);
        p += n;
        return s;
    }
};

void put_value(std::vector<char> & b, const popo::type::Value & v)
{
    using popo::type::ValueType;
    auto t = v.datatype();
    put_u8(b, static_cast<uint8_t>(t));
    switch (t)
    {
        case ValueType::Null:
            break;
        case ValueType::Bool:
            put_u8(b, v.boolean() ? 1 : 0);
            break;
        case ValueType::Char:
        case ValueType::Uint8:
            put_u8(b, v.u8());
            break;
        case ValueType::Int8:
            put_u8(b, static_cast<uint8_t>(v.i8()));
            break;
        case ValueType::Uint16: {
            uint16_t x = v.u16();
            put_u8(b, x & 0xFF);
            put_u8(b, x >> 8);
            break;
        }
        case ValueType::Int16: {
            uint16_t x = static_cast<uint16_t>(v.i16());
            put_u8(b, x & 0xFF);
            put_u8(b, x >> 8);
            break;
        }
        case ValueType::Uint32:
            put_u32(b, v.u32());
            break;
        case ValueType::Int32:
            put_i32(b, v.i32());
            break;
        case ValueType::Float32: {
            float f = v.f32();
            uint32_t x = 0;
            std::memcpy(&x, &f, 4);
            put_u32(b, x);
            break;
        }
        case ValueType::Uint64:
            put_u64(b, v.u64());
            break;
        case ValueType::Int64:
            put_u64(b, static_cast<uint64_t>(v.i64()));
            break;
        case ValueType::Float64: {
            double d = v.f64();
            uint64_t x = 0;
            std::memcpy(&x, &d, 8);
            put_u64(b, x);
            break;
        }
        case ValueType::String:
            put_str(b, v.string_ref());
            break;
    }
}

popo::type::Value get_value(Reader & r)
{
    using popo::type::ValueType;
    auto t = static_cast<ValueType>(r.u8());
    if (!r.ok)
        return popo::type::Value();
    switch (t)
    {
        case ValueType::Null:
            return popo::type::Value();
        case ValueType::Bool:
            return popo::type::Value(ValueType::Bool, r.u8() != 0);
        case ValueType::Char:
            return popo::type::Value(ValueType::Char, static_cast<uint8_t>(r.u8()));
        case ValueType::Uint8:
            return popo::type::Value(ValueType::Uint8, static_cast<uint8_t>(r.u8()));
        case ValueType::Int8:
            return popo::type::Value(ValueType::Int8, static_cast<int8_t>(r.u8()));
        case ValueType::Uint16: {
            uint16_t x = r.u8();
            x |= static_cast<uint16_t>(r.u8()) << 8;
            return popo::type::Value(ValueType::Uint16, x);
        }
        case ValueType::Int16: {
            uint16_t x = r.u8();
            x |= static_cast<uint16_t>(r.u8()) << 8;
            return popo::type::Value(ValueType::Int16, static_cast<int16_t>(x));
        }
        case ValueType::Uint32:
            return popo::type::Value(ValueType::Uint32, r.u32());
        case ValueType::Int32:
            return popo::type::Value(ValueType::Int32, r.i32());
        case ValueType::Float32: {
            uint32_t x = r.u32();
            float f = 0.0f;
            std::memcpy(&f, &x, 4);
            return popo::type::Value(ValueType::Float32, f);
        }
        case ValueType::Uint64:
            return popo::type::Value(ValueType::Uint64, r.u64());
        case ValueType::Int64:
            return popo::type::Value(ValueType::Int64, static_cast<int64_t>(r.u64()));
        case ValueType::Float64: {
            uint64_t x = r.u64();
            double d = 0.0;
            std::memcpy(&d, &x, 8);
            return popo::type::Value(ValueType::Float64, d);
        }
        case ValueType::String:
            return popo::type::Value(ValueType::String, r.str());
    }
    r.ok = false;
    return popo::type::Value();
}

} // namespace

std::vector<char> serialize_schema(const Schema & schema)
{
    std::vector<char> b;
    put_i32(b, schema.id_);
    put_str(b, schema.table_name_);
    put_i32(b, schema.primary_key_col_idx_);
    put_u32(b, static_cast<uint32_t>(schema.columns_.size()));
    for (const auto & col : schema.columns_)
    {
        put_str(b, col.column_name_);
        uint8_t flags = (col.is_primary_key_ ? 1 : 0) | (col.is_nullable_ ? 2 : 0) | (col.is_unique_ ? 4 : 0);
        put_u8(b, flags);
        put_u8(b, static_cast<uint8_t>(col.data_type_));
        put_i32(b, col.type_len_);
        put_u8(b, col.default_value_.has_value() ? 1 : 0);
        if (col.default_value_)
            put_value(b, *col.default_value_);
    }
    put_u32(b, static_cast<uint32_t>(schema.indexes_.size()));
    for (const auto & idx : schema.indexes_)
    {
        put_str(b, idx.index_name_);
        put_u8(b, idx.is_unique_ ? 1 : 0);
        put_u32(b, static_cast<uint32_t>(idx.index_cols_.size()));
        for (int32_t c : idx.index_cols_)
            put_i32(b, c);
    }
    return b;
}

popo::base::Result<Schema> deserialize_schema(const char * data, uint32_t len)
{
    if (data == nullptr && len > 0)
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "schema_io: null data");
    }
    Reader r{data, data != nullptr ? data + len : nullptr, true};
    Schema schema;
    schema.id_ = r.i32();
    schema.table_name_ = r.str();
    schema.primary_key_col_idx_ = r.i32();
    uint32_t ncols = r.u32();
    for (uint32_t i = 0; r.ok && i < ncols; ++i)
    {
        Column col;
        col.column_name_ = r.str();
        uint8_t flags = r.u8();
        col.is_primary_key_ = (flags & 1) != 0;
        col.is_nullable_ = (flags & 2) != 0;
        col.is_unique_ = (flags & 4) != 0;
        col.data_type_ = static_cast<popo::type::ValueType>(r.u8());
        col.type_len_ = r.i32();
        if (r.u8() != 0)
            col.default_value_ = get_value(r);
        schema.columns_.push_back(std::move(col));
    }
    uint32_t nidx = r.u32();
    for (uint32_t i = 0; r.ok && i < nidx; ++i)
    {
        SchemaIndex idx;
        idx.index_name_ = r.str();
        idx.is_unique_ = r.u8() != 0;
        uint32_t ncol = r.u32();
        for (uint32_t j = 0; r.ok && j < ncol; ++j)
            idx.index_cols_.push_back(r.i32());
        schema.indexes_.push_back(std::move(idx));
    }
    if (!r.ok)
    {
        return popo::base::make_error(popo::base::Errno::InternalErr, "schema_io: truncated");
    }
    return schema;
}

} // namespace popo::catalog
