#include "executor/spill/temp_file_manager.h"

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <system_error>

namespace popo::executor
{

TempFileManager::TempFileManager(std::filesystem::path root)
{
    if (root.empty())
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec);
        if (ec)
            return;
    }
    static std::atomic<uint64_t> next_id{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = root / std::format("popodb-query-{}-{}", stamp, next_id.fetch_add(1, std::memory_order_relaxed));
}

TempFileManager::~TempFileManager()
{
    if (created_dir_ && !directory_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
}

popo::base::Result<std::filesystem::path> TempFileManager::create(std::string_view purpose)
{
    if (directory_.empty())
        return popo::base::make_error(popo::base::Errno::IoErr, "could not create query temp directory");
    if (!created_dir_)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec)
        {
            directory_.clear();
            return popo::base::make_error(popo::base::Errno::IoErr, "could not create query temp directory");
        }
        created_dir_ = true;
    }
    auto path = directory_ / std::format("{}-{}.spill", purpose, files_created_++);
    std::ofstream probe(path, std::ios::binary | std::ios::trunc);
    if (!probe)
        return popo::base::make_error(popo::base::Errno::IoErr, "could not create spill file: " + path.string());
    return path;
}

} // namespace popo::executor
