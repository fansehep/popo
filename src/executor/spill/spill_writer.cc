#include "executor/spill/spill_writer.h"

#include <limits>
#include <string>

#include "executor/spill/spill_codec.h"

namespace popo::executor
{

namespace
{

popo::base::Result<void> io_error(std::string message)
{
    return popo::base::make_error(popo::base::Errno::IoErr, std::move(message));
}

} // namespace

SpillWriter::SpillWriter(const std::filesystem::path & path, TempFileManager * owner)
    : out_(path, std::ios::binary | std::ios::trunc), owner_(owner)
{
}

popo::base::Result<void> SpillWriter::write(const popo::type::Row & row)
{
    if (!out_)
        return io_error("spill output is not open");
    auto encoded = encode_spill_row(row);
    if (!encoded)
        return std::unexpected(encoded.error());
    if (encoded->size() > std::numeric_limits<uint32_t>::max())
        return io_error("encoded spill row is too large");
    const uint32_t size = static_cast<uint32_t>(encoded->size());
    out_.write(reinterpret_cast<const char *>(&size), sizeof(size));
    out_.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
    if (!out_)
        return io_error("failed writing spill row");
    if (owner_)
        owner_->add_bytes_written(sizeof(size) + size);
    return {};
}

popo::base::Result<void> SpillWriter::close()
{
    if (!closed_)
    {
        out_.flush();
        closed_ = true;
        if (!out_)
            return io_error("failed flushing spill file");
        out_.close();
    }
    return {};
}

} // namespace popo::executor
