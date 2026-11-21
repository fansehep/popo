#include "executor/spill/spill_reader.h"

#include <cstdint>
#include <vector>

#include "executor/spill/spill_codec.h"

namespace popo::executor
{

using popo::type::Row;

SpillReader::SpillReader(const std::filesystem::path & path) : in_(path, std::ios::binary)
{
}

popo::base::Result<std::optional<Row>> SpillReader::next()
{
    if (!in_)
    {
        if (in_.eof())
            return std::optional<Row>{};
        return popo::base::make_error(popo::base::Errno::IoErr, "spill input is not open");
    }
    uint32_t size = 0;
    in_.read(reinterpret_cast<char *>(&size), sizeof(size));
    if (in_.eof() && in_.gcount() == 0)
        return std::optional<Row>{};
    if (!in_)
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill record header");
    std::vector<char> data(size);
    in_.read(data.data(), static_cast<std::streamsize>(size));
    if (!in_)
        return popo::base::make_error(popo::base::Errno::IoErr, "truncated spill record");
    auto decoded = decode_spill_row(data);
    if (!decoded)
        return std::unexpected(decoded.error());
    return std::optional<Row>(std::move(*decoded));
}

} // namespace popo::executor
