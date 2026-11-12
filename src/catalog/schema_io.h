#pragma once
#include <cstdint>
#include <vector>
#include "src/base/error.h"
#include "src/catalog/schema.h"

namespace popo::catalog
{

// Compact, self-describing encoding of a Schema for the durable catalog and the
// CREATE_TABLE WAL record. No dependency on the storage layer.
std::vector<char> serialize_schema(const Schema & schema);
popo::base::Result<Schema> deserialize_schema(const char * data, uint32_t len);

} // namespace popo::catalog
