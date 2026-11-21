#pragma once

#include <filesystem>
#include <fstream>
#include <optional>

#include "base/error.h"
#include "type/tuple.h"

namespace popo::executor
{

class SpillReader
{
public:
    explicit SpillReader(const std::filesystem::path & path);
    // nullopt is clean EOF.
    popo::base::Result<std::optional<popo::type::Row>> next();

private:
    std::ifstream in_;
};

} // namespace popo::executor
