#pragma once

#include <cstddef>
#include <cstdint>
#include "src/base/error.h"

namespace neon::storage
{

// Synchronous pread / pwrite. A single io_uring_enter on this kernel is much
// slower than pwrite for one-commit writes, so durability uses pwrite plus
// fsync or fdatasync.
class IoRing
{
public:
    IoRing() = default;

    IoRing(const IoRing &) = delete;
    IoRing & operator=(const IoRing &) = delete;

    neon::base::Result<void> read_at(int fd, void * buf, uint32_t len, uint64_t offset);
    neon::base::Result<void> write_at(int fd, const void * buf, uint32_t len, uint64_t offset);
    neon::base::Result<void> fsync(int fd);

    struct Write
    {
        const void * data = nullptr;
        uint32_t len = 0;
        uint64_t offset = 0;
    };
    neon::base::Result<void> sync_writes(int fd, const Write * writes, size_t count, bool data_sync);
};

} // namespace neon::storage
