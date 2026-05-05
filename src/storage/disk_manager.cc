#include "src/storage/disk_manager.h"
#include <cstring>

namespace neon::storage
{

DiskManager::DiskManager(const std::string & data_dir) : data_dir_(data_dir)
{
    // Create data directory if it doesn't exist
    std::filesystem::create_directories(data_dir_);

    // Scan existing .ndb files to populate next_page_ids_
    if (std::filesystem::exists(data_dir_))
    {
        for (const auto & entry : std::filesystem::directory_iterator(data_dir_))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".ndb")
            {
                std::string table_name = entry.path().stem().string();
                auto file_size = entry.file_size();
                page_id_t num_pages = static_cast<page_id_t>(file_size / PAGE_SIZE);
                // next_page_id = num_pages + 1 (page IDs start from 1)
                next_page_ids_[table_name] = num_pages + 1;
            }
        }
    }
}

DiskManager::~DiskManager() = default;

std::string DiskManager::table_file_path(const std::string & table_name) const
{
    return data_dir_ + "/" + table_name + ".ndb";
}

void DiskManager::ensure_file_exists(const std::string & table_name)
{
    std::string path = table_file_path(table_name);
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("Table file does not exist: " + path);
    }
}

neon::base::Result<void> DiskManager::create_table_file(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = table_file_path(table_name);

    if (std::filesystem::exists(path))
    {
        return std::unexpected(
            neon::base::Error(static_cast<int32_t>(neon::base::TableAlreadyExists), "Table file already exists: " + path));
    }

    // Create empty file
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open())
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to create table file: " + path));
    }
    ofs.close();

    // Page IDs start from 1 (0 is INVALID_PAGE_ID)
    next_page_ids_[table_name] = 1;
    return {};
}

void DiskManager::remove_table_file(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = table_file_path(table_name);

    if (std::filesystem::exists(path))
    {
        std::filesystem::remove(path);
    }
    next_page_ids_.erase(table_name);
}

neon::base::Result<void> DiskManager::read_page(const std::string & table_name, page_id_t page_id, char * dest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_file_exists(table_name);

    std::string path = table_file_path(table_name);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
    {
        return std::unexpected(
            neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to open table file for reading: " + path));
    }

    // Page IDs start from 1, so offset = (page_id - 1) * PAGE_SIZE
    auto offset = static_cast<std::streamoff>((page_id - 1)) * PAGE_SIZE;
    auto file_size = static_cast<std::streamoff>(std::filesystem::file_size(path));

    if (offset >= file_size)
    {
        // Page doesn't exist yet, fill with zeros
        std::memset(dest, 0, PAGE_SIZE);
        return {};
    }

    ifs.seekg(offset);
    ifs.read(dest, PAGE_SIZE);

    // If we read fewer bytes than PAGE_SIZE (partial page at end), zero-fill the rest
    auto bytes_read = ifs.gcount();
    if (bytes_read < PAGE_SIZE)
    {
        std::memset(dest + bytes_read, 0, PAGE_SIZE - bytes_read);
    }
    return {};
}

neon::base::Result<void> DiskManager::write_page(const std::string & table_name, page_id_t page_id, const char * src)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_file_exists(table_name);

    std::string path = table_file_path(table_name);
    // Open in read+write mode to avoid truncation
    std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!fs.is_open())
    {
        return std::unexpected(
            neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to open table file for writing: " + path));
    }

    auto offset = static_cast<std::streamoff>((page_id - 1)) * PAGE_SIZE;
    fs.seekp(offset);
    fs.write(src, PAGE_SIZE);
    fs.flush();
    return {};
}

neon::base::Result<page_id_t> DiskManager::allocate_page(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_file_exists(table_name);

    auto it = next_page_ids_.find(table_name);
    if (it == next_page_ids_.end())
    {
        return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Table not tracked: " + table_name));
    }

    page_id_t new_page_id = it->second;
    it->second++;

    // Extend the file by writing a zero page
    std::string path = table_file_path(table_name);
    std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!fs.is_open())
    {
        return std::unexpected(
            neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "Failed to open table file for allocation: " + path));
    }

    auto offset = static_cast<std::streamoff>((new_page_id - 1)) * PAGE_SIZE;
    fs.seekp(offset);
    char zero_page[PAGE_SIZE] = {};
    fs.write(zero_page, PAGE_SIZE);
    fs.flush();

    return new_page_id;
}

page_id_t DiskManager::page_count(const std::string & table_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = table_file_path(table_name);

    if (!std::filesystem::exists(path))
    {
        return 0;
    }

    auto file_size = std::filesystem::file_size(path);
    return static_cast<page_id_t>(file_size / PAGE_SIZE);
}

bool DiskManager::table_file_exists(const std::string & table_name) const
{
    return std::filesystem::exists(table_file_path(table_name));
}

} // namespace neon::storage
