#pragma once
#include <expected>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include "src/base/error.h"
#include "src/storage/common.h"

namespace neon::storage
{

// Manages one data file per table
class DiskManager
{
public:
    explicit DiskManager(const std::string & data_dir);
    ~DiskManager();

    // Create data file for a new table
    neon::base::Result<void> create_table_file(const std::string & table_name);

    // Remove data file for a table
    void remove_table_file(const std::string & table_name);

    // Read a page from the table's data file
    neon::base::Result<void> read_page(const std::string & table_name, page_id_t page_id, char * dest);

    // Write a page to the table's data file
    neon::base::Result<void> write_page(const std::string & table_name, page_id_t page_id, const char * src);

    // Allocate a new page (extends file), returns new page_id
    neon::base::Result<page_id_t> allocate_page(const std::string & table_name);

    // Get number of pages in a table file
    page_id_t page_count(const std::string & table_name);

    // Check if table file exists
    bool table_file_exists(const std::string & table_name) const;

    const std::string & data_dir() const { return data_dir_; }

private:
    std::string data_dir_;
    std::mutex mutex_;

    // Track next page_id per table
    std::unordered_map<std::string, page_id_t> next_page_ids_;

    std::string table_file_path(const std::string & table_name) const;
    void ensure_file_exists(const std::string & table_name);
};

} // namespace neon::storage
