#pragma once

#include <optional>
#include <string>
#include "src/type/value.h"


namespace neon::catalog
{


struct Column
{
    // 列的名称
    std::string column_name_;
    // 是否是主键
    bool is_primary_key_ = false;
    // 是否可空
    bool is_nullable_ = true;
    // 默认值
    std::optional<type::Value> default_value_;
    // 数据类型
    type::ValueType data_type_ = type::ValueType::Null;
    // 数据类型长度限制
    int32_t type_len_ = 0;
    // 是否唯一
    bool is_unique_ = false;
};

}
