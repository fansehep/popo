#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "src/base/error.h"

namespace popo::storage
{

// Synchronous facade over one io_uring instance. Every call submits its SQEs and
// waits for the completions, so callers keep blocking semantics while a batch
// still costs one submission. DiskManager serializes the calls with its
// mutex, so the ring always has a single issuer.
class IoRing
{
public:
    IoRing();
    ~IoRing();

    IoRing(const IoRing &) = delete;
    IoRing & operator=(const IoRing &) = delete;

    popo::base::Result<void> read_at(int fd, void * buf, uint32_t len, uint64_t offset);
    popo::base::Result<void> write_at(int fd, const void * buf, uint32_t len, uint64_t offset);
    popo::base::Result<void> fsync(int fd);

    struct Write
    {
        const void * data = nullptr;
        uint32_t len = 0;
        uint64_t offset = 0;
    };

    // Queues every write in one submission, then waits for all completions.
    popo::base::Result<void> write_batch(int fd, const Write * writes, size_t count);
    // write_batch followed by fsync, or fdatasync when data_sync is set.
    popo::base::Result<void> sync_writes(int fd, const Write * writes, size_t count, bool data_sync);
    // Chains the write to its fsync with IOSQE_IO_LINK, so the durability
    // barrier costs one submission and one wait instead of two round trips.
    popo::base::Result<void> write_and_fsync(int fd, const void * buf, uint32_t len, uint64_t offset, bool data_sync);

private:
    struct Impl;

    popo::base::Result<void> fsync_impl(int fd, bool data_sync);

    std::unique_ptr<Impl> impl_;
};

} // namespace popo::storage
