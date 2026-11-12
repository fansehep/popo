#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "src/base/error.h"
#include "src/catalog/column.h"
#include "src/type/tuple.h"


namespace popo::catalog
{

struct SchemaIndex
{
    // 索引名称
    std::string index_name_;
    // 索引列
    std::vector<int32_t> index_cols_;
    // 是否唯一
    bool is_unique_;
};


using SchemaId = int32_t;

struct Schema
{
    // 表的Id
    SchemaId id_ = 0;
    // 列的数量
    std::vector<Column> columns_;
    // 表的名称
    std::string table_name_;
    // 主键列（单列主键时 >= 0；复合主键或无显式主键时为 -1）
    int32_t primary_key_col_idx_ = -1;
    // 该表的所有索引
    std::vector<SchemaIndex> indexes_;

    static popo::type::Value coerce_value(popo::type::ValueType target, const popo::type::Value & v)
    {
        using popo::type::Value;
        using popo::type::ValueType;
        if (v.datatype() == ValueType::Null || v.datatype() == target)
        {
            return v;
        }
        auto as_i64 = [](const Value & x) -> int64_t
        {
            switch (x.datatype())
            {
                case ValueType::Int8: return x.i8();
                case ValueType::Int16: return x.i16();
                case ValueType::Int32: return x.i32();
                case ValueType::Int64: return x.i64();
                case ValueType::Uint8: return static_cast<int64_t>(x.u8());
                case ValueType::Uint16: return static_cast<int64_t>(x.u16());
                case ValueType::Uint32: return static_cast<int64_t>(x.u32());
                case ValueType::Uint64: return static_cast<int64_t>(x.u64());
                case ValueType::Bool: return x.boolean() ? 1 : 0;
                case ValueType::String: return popo::base::parse_i64(x.string_ref()).value_or(0);
                default: return static_cast<int64_t>(x.to_f64());
            }
        };
        auto as_f64 = [](const Value & x) -> double
        {
            if (x.is_numeric())
                return x.to_f64();
            if (x.datatype() == ValueType::Bool)
                return x.boolean() ? 1.0 : 0.0;
            if (x.datatype() == ValueType::String)
                return popo::base::parse_f64(x.string_ref()).value_or(0.0);
            return 0.0;
        };
        switch (target)
        {
            case ValueType::Bool:
                return Value(ValueType::Bool, v.is_numeric() ? v.to_f64() != 0.0 : !v.to_val_string().empty());
            case ValueType::Int8:
                return Value(ValueType::Int8, static_cast<int8_t>(as_i64(v)));
            case ValueType::Uint8:
                return Value(ValueType::Uint8, static_cast<uint8_t>(as_i64(v)));
            case ValueType::Int16:
                return Value(ValueType::Int16, static_cast<int16_t>(as_i64(v)));
            case ValueType::Uint16:
                return Value(ValueType::Uint16, static_cast<uint16_t>(as_i64(v)));
            case ValueType::Int32:
                return Value(ValueType::Int32, static_cast<int32_t>(as_i64(v)));
            case ValueType::Uint32:
                return Value(ValueType::Uint32, static_cast<uint32_t>(as_i64(v)));
            case ValueType::Int64:
                return Value(ValueType::Int64, as_i64(v));
            case ValueType::Uint64:
                return Value(ValueType::Uint64, static_cast<uint64_t>(as_i64(v)));
            case ValueType::Float32:
                return Value(ValueType::Float32, static_cast<float>(as_f64(v)));
            case ValueType::Float64:
                return Value(ValueType::Float64, as_f64(v));
            case ValueType::Char:
                return Value(
                    ValueType::Char,
                    (v.datatype() == ValueType::String && !v.string_ref().empty())
                        ? static_cast<uint8_t>(v.string_ref()[0])
                        : static_cast<uint8_t>(as_i64(v)));
            case ValueType::String:
                return Value(ValueType::String, v.datatype() == ValueType::String ? v.string_ref() : v.to_val_string());
            default:
                return v;
        }
    }

    popo::type::Row coerce_row(const popo::type::Row & row) const
    {
        popo::type::Row out;
        out.reserve_values(columns_.size());
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            if (i < row.values_.size())
                out.append_value(coerce_value(columns_[i].data_type_, row.values_[i]));
            else
                out.append_value(popo::type::Value());
        }
        return out;
    }

    bool has_primary_key() const
    {
        if (primary_key_col_idx_ >= 0)
            return true;
        for (const auto & col : columns_)
        {
            if (col.is_primary_key_)
                return true;
        }
        return false;
    }

    std::vector<size_t> key_column_indices() const
    {
        std::vector<size_t> idxs;
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            if (columns_[i].is_primary_key_)
                idxs.push_back(i);
        }
        if (idxs.empty())
        {
            idxs.reserve(columns_.size());
            for (size_t i = 0; i < columns_.size(); ++i)
                idxs.push_back(i);
        }
        return idxs;
    }

    static void encode_key_part(std::string & out, popo::type::ValueType col_type, const popo::type::Value & raw)
    {
        using popo::type::ValueType;
        auto val = coerce_value(col_type, raw);
        if (val.datatype() == ValueType::Null)
        {
            out.push_back('\x00');
            return;
        }
        out.push_back('\x01');
        auto put_be16 = [&](uint16_t x)
        {
            out.push_back(static_cast<char>((x >> 8) & 0xFF));
            out.push_back(static_cast<char>(x & 0xFF));
        };
        auto put_be32 = [&](uint32_t x)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<char>((x >> shift) & 0xFF));
        };
        auto put_be64 = [&](uint64_t x)
        {
            for (int shift = 56; shift >= 0; shift -= 8)
                out.push_back(static_cast<char>((x >> shift) & 0xFF));
        };
        switch (col_type)
        {
            case ValueType::Bool:
                out.push_back(val.boolean() ? '\x01' : '\x00');
                break;
            case ValueType::Uint8:
            case ValueType::Char:
                out.push_back(static_cast<char>(val.u8()));
                break;
            case ValueType::Int8:
                out.push_back(static_cast<char>(static_cast<uint8_t>(val.i8()) ^ 0x80u));
                break;
            case ValueType::Uint16:
                put_be16(val.u16());
                break;
            case ValueType::Int16:
                put_be16(static_cast<uint16_t>(val.i16()) ^ 0x8000u);
                break;
            case ValueType::Uint32:
                put_be32(val.u32());
                break;
            case ValueType::Int32:
                put_be32(static_cast<uint32_t>(val.i32()) ^ 0x80000000u);
                break;
            case ValueType::Uint64:
                put_be64(val.u64());
                break;
            case ValueType::Int64:
                put_be64(static_cast<uint64_t>(val.i64()) ^ (1ULL << 63));
                break;
            case ValueType::Float32: {
                float f = (val.f32() == 0.0f) ? 0.0f : val.f32();
                uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                uint32_t mapped = (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
                put_be32(mapped);
                break;
            }
            case ValueType::Float64: {
                double d = (val.f64() == 0.0) ? 0.0 : val.f64();
                uint64_t bits = 0;
                std::memcpy(&bits, &d, 8);
                uint64_t mapped = (bits & (1ULL << 63)) ? ~bits : (bits ^ (1ULL << 63));
                put_be64(mapped);
                break;
            }
            case ValueType::String: {
                for (unsigned char c : val.string_ref())
                {
                    if (c == 0)
                    {
                        out.push_back('\x00');
                        out.push_back('\xFF');
                    }
                    else
                    {
                        out.push_back(static_cast<char>(c));
                    }
                }
                out.push_back('\x00');
                out.push_back('\x00');
                break;
            }
            default:
                break;
        }
    }

    static popo::type::Value decode_key_part(const std::string & in, size_t & pos, popo::type::ValueType col_type)
    {
        using popo::type::Value;
        using popo::type::ValueType;
        if (pos >= in.size())
            return Value();
        uint8_t tag = static_cast<uint8_t>(in[pos++]);
        if (tag == 0)
            return Value();
        auto get_u8 = [&]() -> uint8_t { return pos < in.size() ? static_cast<uint8_t>(in[pos++]) : 0; };
        auto get_be16 = [&]() -> uint16_t
        {
            uint16_t hi = get_u8();
            uint16_t lo = get_u8();
            return static_cast<uint16_t>((hi << 8) | lo);
        };
        auto get_be32 = [&]() -> uint32_t
        {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v = (v << 8) | get_u8();
            return v;
        };
        auto get_be64 = [&]() -> uint64_t
        {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v = (v << 8) | get_u8();
            return v;
        };
        switch (col_type)
        {
            case ValueType::Bool:
                return Value(ValueType::Bool, get_u8() != 0);
            case ValueType::Uint8:
                return Value(ValueType::Uint8, get_u8());
            case ValueType::Char:
                return Value(ValueType::Char, get_u8());
            case ValueType::Int8:
                return Value(ValueType::Int8, static_cast<int8_t>(get_u8() ^ 0x80u));
            case ValueType::Uint16:
                return Value(ValueType::Uint16, get_be16());
            case ValueType::Int16:
                return Value(ValueType::Int16, static_cast<int16_t>(get_be16() ^ 0x8000u));
            case ValueType::Uint32:
                return Value(ValueType::Uint32, get_be32());
            case ValueType::Int32:
                return Value(ValueType::Int32, static_cast<int32_t>(get_be32() ^ 0x80000000u));
            case ValueType::Uint64:
                return Value(ValueType::Uint64, get_be64());
            case ValueType::Int64:
                return Value(ValueType::Int64, static_cast<int64_t>(get_be64() ^ (1ULL << 63)));
            case ValueType::Float32: {
                uint32_t mapped = get_be32();
                uint32_t bits = (mapped & 0x80000000u) ? (mapped ^ 0x80000000u) : ~mapped;
                float f = 0.0f;
                std::memcpy(&f, &bits, 4);
                return Value(ValueType::Float32, f);
            }
            case ValueType::Float64: {
                uint64_t mapped = get_be64();
                uint64_t bits = (mapped & (1ULL << 63)) ? (mapped ^ (1ULL << 63)) : ~mapped;
                double d = 0.0;
                std::memcpy(&d, &bits, 8);
                return Value(ValueType::Float64, d);
            }
            case ValueType::String: {
                std::string s;
                while (pos < in.size())
                {
                    uint8_t c = get_u8();
                    if (c == 0)
                    {
                        uint8_t next = get_u8();
                        if (next == 0)
                            break;
                        s.push_back('\x00');
                    }
                    else
                    {
                        s.push_back(static_cast<char>(c));
                    }
                }
                return Value(ValueType::String, s);
            }
            default:
                return Value();
        }
    }

    popo::type::Value extract_pk(const popo::type::Row & row) const
    {
        if (primary_key_col_idx_ >= 0)
        {
            auto idx = static_cast<size_t>(primary_key_col_idx_);
            if (idx >= row.values_.size())
                return popo::type::Value();
            return coerce_value(columns_[idx].data_type_, row.values_[idx]);
        }
        auto idxs = key_column_indices();
        std::string encoded;
        encoded.reserve(idxs.size() * 9);
        for (size_t idx : idxs)
        {
            const auto & val = idx < row.values_.size() ? row.values_[idx] : popo::type::Value();
            encode_key_part(encoded, columns_[idx].data_type_, val);
        }
        return popo::type::Value(popo::type::ValueType::String, encoded);
    }

    void fill_pk_row(const popo::type::Value & pk, popo::type::Row & key_row) const
    {
        key_row.values_.clear();
        key_row.reserve_values(columns_.size());
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            key_row.append_value(popo::type::Value());
        }
        if (primary_key_col_idx_ >= 0)
        {
            auto idx = static_cast<size_t>(primary_key_col_idx_);
            if (idx < columns_.size())
                key_row.values_[idx] = coerce_value(columns_[idx].data_type_, pk);
            return;
        }
        if (pk.datatype() != popo::type::ValueType::String)
            return;
        const auto & bytes = pk.string_ref();
        size_t pos = 0;
        for (size_t idx : key_column_indices())
        {
            key_row.values_[idx] = decode_key_part(bytes, pos, columns_[idx].data_type_);
        }
    }
};


using SchemaRef = std::shared_ptr<Schema>;

}
