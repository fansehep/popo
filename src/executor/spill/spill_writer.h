#pragma once

#include <filesystem>
#include <fstream>

#include "base/error.h"
#include "executor/spill/temp_file_manager.h"
#include "type/tuple.h"

namespace popo::executor
{

// Stable, typed binary encoding writer for spilled row streams.
class SpillWriter
{
public:
    SpillWriter(const std::filesystem::path & path, TempFileManager * owner = nullptr);
    popo::base::Result<void> write(const popo::type::Row & row);
    popo::base::Result<void> close();

private:
    std::ofstream out_;
    TempFileManager * owner_;
    bool closed_ = false;
};

} // namespace popo::executor
