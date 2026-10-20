#include "src/type/value.h"

namespace popo::type
{

const char * type_to_string(ValueType type)
{
    switch (type)
    {
        case ValueType::Null:
            return "Null";
        case ValueType::Uint8:
            return "Uint8";
        case ValueType::Uint16:
            return "Uint16";
        case ValueType::Uint32:
            return "Uint32";
        case ValueType::Uint64:
            return "Uint64";
        case ValueType::Int8:
            return "Int8";
        case ValueType::Int16:
            return "Int16";
        case ValueType::Int32:
            return "Int32";
        case ValueType::Int64:
            return "Int64";
        case ValueType::Float32:
            return "Float32";
        case ValueType::Float64:
            return "Float64";
        case ValueType::String:
            return "String";
        case ValueType::Bool:
            return "Bool";
        case ValueType::Char:
            return "Char";
        default:
            return "Unknown";
    }
}

} // namespace popo::type