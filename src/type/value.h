#pragma once


#include <cstdint>
#include <format>
#include <string>
#include "src/base/macro.h"

namespace neon::type
{

enum class ValueType : uint8_t
{
    Null,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    Int8,
    Int16,
    Int32,
    Int64,
    Float32,
    Float64,
    String,
    Bool,
    Char,
};

const char * type_to_string(ValueType type);


class Value
{
public:
    Value() { type_ = ValueType::Null; }

    // 拷贝构造函数
    Value(const Value & other) : type_(other.type_)
    {
        switch (type_)
        {
            case ValueType::String:
                value_.str_val_ = new std::string(*other.value_.str_val_);
                break;
            default:
                value_ = other.value_;
                break;
        }
    }

    // 赋值操作符
    Value & operator=(const Value & other)
    {
        if (this != &other)
        {
            // 先清理当前资源
            if (type_ == ValueType::String)
            {
                delete value_.str_val_;
            }

            type_ = other.type_;
            switch (type_)
            {
                case ValueType::String:
                    value_.str_val_ = new std::string(*other.value_.str_val_);
                    break;
                default:
                    value_ = other.value_;
                    break;
            }
        }
        return *this;
    }

    static bool is_type_equal(const Value & a, const Value & b) { return a.type_ == b.type_; }
    /**
     * @brief 判断Value是否是数值类型
     * @return
     */
    bool is_numeric() const noexcept;

    [[nodiscard]] ValueType datatype() const { return type_; }

    auto set_type(const ValueType new_type) -> void { type_ = new_type; }

    auto type_str() -> std::string { return type_to_string(type_); }

    explicit Value(const ValueType new_type) : type_(new_type) { }

    explicit Value(const ValueType type, const std::string & arg) : type_(type)
    {
        ASSERT((type == ValueType::Char || type == ValueType::String), "expected type: String or Char, but:{}", type_to_string(type));
        value_.str_val_ = new std::string(arg);
    }
    explicit Value(const ValueType new_type, const char * arg) : type_(new_type)
    {
        ASSERT(
            (new_type == ValueType::Char || new_type == ValueType::String),
            "expected type: String or Char, but:{}",
            type_to_string(new_type));
        value_.str_val_ = new std::string(arg);
    }

    explicit Value(const ValueType new_type, const char arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Char, "expected type Char, but:{}", type_to_string(new_type));
        value_.u8_val_ = static_cast<uint8_t>(arg);
    }

    explicit Value(const ValueType new_type, const uint8_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Uint8, "expected type Uint8, but:{}", type_to_string(new_type));
        value_.u8_val_ = arg;
    }

    explicit Value(const ValueType new_type, const uint16_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Uint16, "expected type Uint16, but:{}", type_to_string(new_type));
        value_.u16_val_ = arg;
    }

    explicit Value(const ValueType new_type, const uint32_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Uint32, "expected type Uint32, but:{}", type_to_string(new_type));
        value_.u32_val_ = arg;
    }

    explicit Value(const ValueType new_type, const uint64_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Uint64, "expected type Uint64, but:{}", type_to_string(new_type));
        value_.u64_val_ = arg;
    }

    explicit Value(const ValueType new_type, const int8_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Int8, "expected type Int8, but:{}", type_to_string(new_type));
        value_.i8_val_ = arg;
    }

    explicit Value(const ValueType new_type, const int16_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Int16, "expected type Int16, but:{}", type_to_string(new_type));
        value_.i16_val_ = arg;
    }

    explicit Value(const ValueType new_type, const int32_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Int32, "expected type Int32, but:{}", type_to_string(new_type));
        value_.i32_val_ = arg;
    }

    explicit Value(const ValueType new_type, const int64_t arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Int64, "expected type Int64, but:{}", type_to_string(new_type));
        value_.i64_val_ = arg;
    }

    explicit Value(const ValueType new_type, const float arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Float32, "expected type Float32, but:{}", type_to_string(new_type));
        value_.f32_val_ = arg;
    }

    explicit Value(const ValueType new_type, const double arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Float64, "expected type Float64, but:{}", type_to_string(new_type));
        value_.f64_val_ = arg;
    }

    explicit Value(const ValueType new_type, const bool arg) : type_(new_type)
    {
        ASSERT(new_type == ValueType::Bool, "expected type Bool, but:{}", type_to_string(new_type));
        value_.bool_val_ = arg;
    }

    ~Value()
    {
        if (type_ == ValueType::String)
        {
            delete value_.str_val_;
        }
    }

    [[nodiscard]] auto u8() const -> uint8_t { return value_.u8_val_; }
    [[nodiscard]] auto u16() const -> uint16_t { return value_.u16_val_; }
    [[nodiscard]] auto u32() const -> uint32_t { return value_.u32_val_; }
    [[nodiscard]] auto u64() const -> uint64_t { return value_.u64_val_; }
    [[nodiscard]] auto i8() const -> int8_t { return value_.i8_val_; }
    [[nodiscard]] auto i16() const -> int16_t { return value_.i16_val_; }
    [[nodiscard]] auto i32() const -> int32_t { return value_.i32_val_; }
    [[nodiscard]] auto i64() const -> int64_t { return value_.i64_val_; }
    [[nodiscard]] auto boolean() const -> bool { return value_.bool_val_; }
    [[nodiscard]] auto f32() const -> float { return value_.f32_val_; }
    [[nodiscard]] auto f64() const -> double { return value_.f64_val_; }
    [[nodiscard]] auto string_ref() const -> std::string & { return *value_.str_val_; }

    // Convert any numeric type to double for cross-type comparison
    [[nodiscard]] double to_f64() const
    {
        switch (type_)
        {
            case ValueType::Uint8:
                return static_cast<double>(value_.u8_val_);
            case ValueType::Uint16:
                return static_cast<double>(value_.u16_val_);
            case ValueType::Uint32:
                return static_cast<double>(value_.u32_val_);
            case ValueType::Uint64:
                return static_cast<double>(value_.u64_val_);
            case ValueType::Int8:
                return static_cast<double>(value_.i8_val_);
            case ValueType::Int16:
                return static_cast<double>(value_.i16_val_);
            case ValueType::Int32:
                return static_cast<double>(value_.i32_val_);
            case ValueType::Int64:
                return static_cast<double>(value_.i64_val_);
            case ValueType::Float32:
                return static_cast<double>(value_.f32_val_);
            case ValueType::Float64:
                return value_.f64_val_;
            case ValueType::Char:
                return static_cast<double>(value_.u8_val_);
            default:
                return 0.0;
        }
    }

    friend bool operator<(const Value & lhs, const Value & rhs)
    {
        if (lhs.is_numeric() && rhs.is_numeric())
        {
            return lhs.to_f64() < rhs.to_f64();
        }
        ASSERT(lhs.type_ == rhs.type_, "Cannot compare different types");
        switch (lhs.type_)
        {
            case ValueType::Uint8:
                return lhs.u8() < rhs.u8();
            case ValueType::Uint16:
                return lhs.u16() < rhs.u16();
            case ValueType::Uint32:
                return lhs.u32() < rhs.u32();
            case ValueType::Uint64:
                return lhs.u64() < rhs.u64();
            case ValueType::Int8:
                return lhs.i8() < rhs.i8();
            case ValueType::Int16:
                return lhs.i16() < rhs.i16();
            case ValueType::Int32:
                return lhs.i32() < rhs.i32();
            case ValueType::Int64:
                return lhs.i64() < rhs.i64();
            case ValueType::Float32:
                return lhs.f32() < rhs.f32();
            case ValueType::Float64:
                return lhs.f64() < rhs.f64();
            case ValueType::Bool:
                return lhs.boolean() < rhs.boolean();
            case ValueType::Char:
                return lhs.u8() < rhs.u8();
            case ValueType::String:
                return lhs.string_ref() < rhs.string_ref();
            default:
                PANIC("Unsupported type for comparison");
                return false;
        }
    }

    friend bool operator>(const Value & lhs, const Value & rhs)
    {
        if (lhs.is_numeric() && rhs.is_numeric())
        {
            return lhs.to_f64() > rhs.to_f64();
        }
        ASSERT(lhs.type_ == rhs.type_, "Cannot compare different types");
        switch (lhs.type_)
        {
            case ValueType::Uint8:
                return lhs.u8() > rhs.u8();
            case ValueType::Uint16:
                return lhs.u16() > rhs.u16();
            case ValueType::Uint32:
                return lhs.u32() > rhs.u32();
            case ValueType::Uint64:
                return lhs.u64() > rhs.u64();
            case ValueType::Int8:
                return lhs.i8() > rhs.i8();
            case ValueType::Int16:
                return lhs.i16() > rhs.i16();
            case ValueType::Int32:
                return lhs.i32() > rhs.i32();
            case ValueType::Int64:
                return lhs.i64() > rhs.i64();
            case ValueType::Float32:
                return lhs.f32() > rhs.f32();
            case ValueType::Float64:
                return lhs.f64() > rhs.f64();
            case ValueType::Bool:
                return lhs.boolean() > rhs.boolean();
            case ValueType::Char:
                return lhs.u8() > rhs.u8();
            case ValueType::String:
                return lhs.string_ref() > rhs.string_ref();
            default:
                PANIC("Unsupported type for comparison");
                return false;
        }
    }

    friend bool operator==(const Value & lhs, const Value & rhs)
    {
        if (lhs.is_numeric() && rhs.is_numeric())
        {
            return lhs.to_f64() == rhs.to_f64();
        }
        ASSERT(lhs.type_ == rhs.type_, "Cannot compare different types");
        switch (lhs.type_)
        {
            case ValueType::Uint8:
                return lhs.u8() == rhs.u8();
            case ValueType::Uint16:
                return lhs.u16() == rhs.u16();
            case ValueType::Uint32:
                return lhs.u32() == rhs.u32();
            case ValueType::Uint64:
                return lhs.u64() == rhs.u64();
            case ValueType::Int8:
                return lhs.i8() == rhs.i8();
            case ValueType::Int16:
                return lhs.i16() == rhs.i16();
            case ValueType::Int32:
                return lhs.i32() == rhs.i32();
            case ValueType::Int64:
                return lhs.i64() == rhs.i64();
            case ValueType::Float32:
                return lhs.f32() == rhs.f32();
            case ValueType::Float64:
                return lhs.f64() == rhs.f64();
            case ValueType::Bool:
                return lhs.boolean() == rhs.boolean();
            case ValueType::Char:
                return lhs.u8() == rhs.u8();
            case ValueType::String:
                return lhs.string_ref() == rhs.string_ref();
            default:
                PANIC("Unsupported type for comparison");
                return false;
        }
    }

    [[nodiscard]] std::string to_val_string() const
    {
        switch (type_)
        {
            case ValueType::Uint8:
                return std::to_string(u8());
            case ValueType::Uint16:
                return std::to_string(u16());
            case ValueType::Uint32:
                return std::to_string(u32());
            case ValueType::Uint64:
                return std::to_string(u64());
            case ValueType::Int8:
                return std::to_string(i8());
            case ValueType::Int16:
                return std::to_string(i16());
            case ValueType::Int32:
                return std::to_string(i32());
            case ValueType::Int64:
                return std::to_string(i64());
            case ValueType::Float32:
                return std::to_string(f32());
            case ValueType::Float64:
                return std::to_string(f64());
            case ValueType::Bool:
                return boolean() ? "true" : "false";
            case ValueType::Char:
                return std::string(1, static_cast<char>(u8()));
            case ValueType::String:
                return string_ref();
            default:
                return "null";
        }
    }

private:
    union Val
    {
        uint8_t u8_val_;
        uint16_t u16_val_;
        uint32_t u32_val_;
        uint64_t u64_val_;
        int8_t i8_val_;
        int16_t i16_val_;
        int32_t i32_val_;
        int64_t i64_val_;
        bool bool_val_;
        float f32_val_;
        double f64_val_;
        std::string * str_val_;
    };

    ValueType type_{ValueType::Null};
    Val value_{};
};

} // namespace neon::type

template <>
struct std::formatter<neon::type::Value> : std::formatter<std::string>
{
    template <typename FormatContext>
    auto format(const neon::type::Value & val, FormatContext & ctx) const
    {
        return std::formatter<std::string>::format(val.to_val_string(), ctx);
    }
};