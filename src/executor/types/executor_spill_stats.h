#pragma once

#include <cstddef>
#include <cstdint>

namespace popo::executor
{

struct ExecutorSpillStats
{
    size_t peak_memory_bytes = 0;
    size_t files_created = 0;
    uint64_t bytes_written = 0;
};

} // namespace popo::executor
