#pragma once


#include <cstdint>
#include <memory>
#include <vector>
#include "src/type/value.h"


namespace neon::type
{


struct Row
{
    [[nodiscard]] std::string to_string() const
    {
        std::string res;
        for (size_t i = 0; i < values_.size(); ++i)
        {
            std::format_to(std::back_inserter(res), "{}", values_[i].to_val_string());
            if (i != values_.size() - 1)
            {
                std::format_to(std::back_inserter(res), ",");
            }
        }
        return res;
    }

    [[nodiscard]] auto len() const -> uint32_t { return values_.size(); }

    auto value_ref(const uint32_t idx) -> neon::type::Value & { return values_[idx]; }

    auto append_value(const neon::type::Value & value) -> void { values_.push_back(value); }

    std::vector<neon::type::Value> values_;

    Row() = default;
};

using RowRef = std::shared_ptr<Row>;


}
