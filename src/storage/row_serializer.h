#pragma once
#include <vector>
#include "src/catalog/schema.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace neon::storage
{

class RowSerializer
{
public:
    // Serialize a Row into bytes based on schema
    static std::vector<char> serialize(const neon::type::Row & row, const neon::catalog::Schema & schema);

    // Deserialize bytes back into a Row based on schema
    static neon::type::Row deserialize(const char * data, uint32_t len, const neon::catalog::Schema & schema);

    // Calculate serialized size without actually serializing
    static uint32_t serialized_size(const neon::type::Row & row, const neon::catalog::Schema & schema);
};

} // namespace neon::storage
