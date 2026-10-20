#pragma once


#include <cstdint>
#include <memory>
#include <vector>
#include "src/type/value.h"


namespace popo::type
{


struct Row
{
    [[nodiscard]] std::string to_string() const
    {
        std::string res;
        // Estimate: ~8 bytes per value + 1 comma per separator
        res.reserve(values_.size() * 9);
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

    auto value_ref(const uint32_t idx) -> popo::type::Value & { return values_[idx]; }
    [[nodiscard]] auto value_ref(const uint32_t idx) const -> const popo::type::Value & { return values_[idx]; }

    auto append_value(const popo::type::Value & value) -> void { values_.push_back(value); }
    auto append_value(popo::type::Value && value) -> void { values_.push_back(std::move(value)); }

    auto reserve_values(size_t n) -> void { values_.reserve(n); }

    std::vector<popo::type::Value> values_;

    Row() = default;
    Row(const Row &) = default;
    Row & operator=(const Row &) = default;
    Row(Row &&) noexcept = default;
    Row & operator=(Row &&) noexcept = default;
};

using RowRef = std::shared_ptr<Row>;


}
