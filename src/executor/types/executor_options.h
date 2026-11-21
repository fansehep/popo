#pragma once

#include <cstddef>
#include <filesystem>

namespace popo::executor
{

struct ExecutorOptions
{
    size_t memory_limit_bytes = 4ULL * 1024 * 1024 * 1024;
    std::filesystem::path temp_directory;
};

} // namespace popo::executor
