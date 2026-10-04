#pragma once

#include <cstdint>
#include <vector>
#include "src/catalog/schema.h"
#include "src/storage/row_serializer.h"
#include "src/type/tuple.h"

namespace neon::storage
{

// Physical tuple. The byte layout is defined by Schema: a null bitmap and
// column payloads, with no per-column type tag. Schema.columns_[i].data_type_
// is what makes those bytes a value. Form at insert, deform when the executor
// needs a neon::type::Value.
class Tuple
{
public:
    Tuple() = default;
    explicit Tuple(std::vector<char> data) : data_(std::move(data)) { }

    static Tuple form(const neon::catalog::Schema & schema, const neon::type::Row & row)
    {
        return Tuple(RowSerializer::serialize(row, schema));
    }

    [[nodiscard]] neon::type::Row deform(const neon::catalog::Schema & schema) const
    {
        return RowSerializer::deserialize(data_.data(), static_cast<uint32_t>(data_.size()), schema);
    }

    [[nodiscard]] const char * data() const { return data_.data(); }
    [[nodiscard]] uint16_t size() const { return static_cast<uint16_t>(data_.size()); }
    [[nodiscard]] const std::vector<char> & bytes() const { return data_; }
    [[nodiscard]] bool empty() const { return data_.empty(); }

private:
    std::vector<char> data_;
};

} // namespace neon::storage
