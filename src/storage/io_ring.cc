#include "src/storage/io_ring.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace neon::storage
{

namespace
{

neon::base::Result<void> fail(const char * what)
{
    return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), std::string(what) + ": " + std::strerror(errno)));
}

} // namespace

neon::base::Result<void> IoRing::read_at(int fd, void * buf, uint32_t len, uint64_t offset)
{
    auto * cursor = static_cast<char *>(buf);
    uint32_t left = len;
    while (left > 0)
    {
        ssize_t n = ::pread(fd, cursor, left, static_cast<off_t>(offset));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return fail("pread");
        }
        if (n == 0)
        {
            std::memset(cursor, 0, left);
            return {};
        }
        cursor += n;
        offset += static_cast<uint64_t>(n);
        left -= static_cast<uint32_t>(n);
    }
    return {};
}

neon::base::Result<void> IoRing::write_at(int fd, const void * buf, uint32_t len, uint64_t offset)
{
    auto * cursor = static_cast<const char *>(buf);
    uint32_t left = len;
    while (left > 0)
    {
        ssize_t n = ::pwrite(fd, cursor, left, static_cast<off_t>(offset));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return fail("pwrite");
        }
        if (n == 0)
        {
            return std::unexpected(neon::base::Error(static_cast<int32_t>(neon::base::InternalErr), "short write"));
        }
        cursor += n;
        offset += static_cast<uint64_t>(n);
        left -= static_cast<uint32_t>(n);
    }
    return {};
}

neon::base::Result<void> IoRing::fsync(int fd)
{
    return sync_writes(fd, nullptr, 0, false);
}

neon::base::Result<void> IoRing::sync_writes(int fd, const Write * writes, size_t count, bool data_sync)
{
    for (size_t i = 0; i < count; ++i)
    {
        auto written = write_at(fd, writes[i].data, writes[i].len, writes[i].offset);
        if (!written)
        {
            return written;
        }
    }
    while (true)
    {
        int rc = data_sync ? ::fdatasync(fd) : ::fsync(fd);
        if (rc == 0)
        {
            return {};
        }
        if (errno != EINTR)
        {
            return fail(data_sync ? "fdatasync" : "fsync");
        }
    }
}

} // namespace neon::storage
