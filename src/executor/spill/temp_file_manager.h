#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "base/error.h"

namespace popo::executor
{

// Owns every file created by one statement. Destruction is the final cleanup
// boundary on success, expected errors, and exceptions from the C++ runtime.
class TempFileManager
{
public:
    explicit TempFileManager(std::filesystem::path root = {});
    ~TempFileManager();
    TempFileManager(const TempFileManager &) = delete;
    TempFileManager & operator=(const TempFileManager &) = delete;

    popo::base::Result<std::filesystem::path> create(std::string_view purpose);
    const std::filesystem::path & directory() const noexcept { return directory_; }
    size_t files_created() const noexcept { return files_created_; }
    uint64_t bytes_written() const noexcept { return bytes_written_; }
    void add_bytes_written(uint64_t bytes) noexcept { bytes_written_ += bytes; }

private:
    std::filesystem::path directory_;
    bool created_dir_ = false;
    size_t files_created_ = 0;
    uint64_t bytes_written_ = 0;
};

} // namespace popo::executor
