#pragma once
#include <vector>
#include "src/catalog/schema.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace popo::storage
{

class RowSerializer
{
public:
    // Serialize a Row into bytes based on schema
    static std::vector<char> serialize(const popo::type::Row & row, const popo::catalog::Schema & schema);

    // Deserialize bytes back into a Row based on schema
    static popo::type::Row deserialize(const char * data, uint32_t len, const popo::catalog::Schema & schema);

    // Calculate serialized size without actually serializing
    static uint32_t serialized_size(const popo::type::Row & row, const popo::catalog::Schema & schema);

    // Decode one column. Other columns are skipped and not allocated.
    static popo::type::Value read_column(const char * data, uint32_t len, const popo::catalog::Schema & schema, int32_t column);
};

} // namespace popo::storage
