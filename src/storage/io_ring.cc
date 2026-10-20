#include "src/storage/io_ring.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace popo::storage
{

namespace
{

// A commit publishes a few hundred compressed pages at most; the SQ is sized so
// one transaction fits without splitting the batch.
constexpr unsigned QUEUE_DEPTH = 256;

popo::base::Result<void> fail(const char * what, int err)
{
    return std::unexpected(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), std::string(what) + ": " + std::strerror(err)));
}

} // namespace

struct IoRing::Impl
{
    io_uring ring{};

    Impl()
    {
        int rc = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
        if (rc < 0)
        {
            throw std::runtime_error(std::string("io_uring_queue_init: ") + std::strerror(-rc));
        }
    }

    ~Impl() { io_uring_queue_exit(&ring); }

    // Reaps one completion and reports it through res. Returns false only when
    // the ring itself failed; the operation result stays in res either way.
    bool wait(int & res)
    {
        io_uring_cqe * cqe = nullptr;
        int rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc < 0)
        {
            err = -rc;
            return false;
        }
        res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        return true;
    }

    int err = 0;
};

IoRing::IoRing() : impl_(std::make_unique<Impl>()) {}

IoRing::~IoRing() = default;

popo::base::Result<void> IoRing::read_at(int fd, void * buf, uint32_t len, uint64_t offset)
{
    auto * cursor = static_cast<char *>(buf);
    uint32_t left = len;
    while (left > 0)
    {
        io_uring_sqe * sqe = io_uring_get_sqe(&impl_->ring);
        if (sqe == nullptr)
        {
            return fail("io_uring_get_sqe", EAGAIN);
        }
        io_uring_prep_read(sqe, fd, cursor, left, offset);
        int rc = io_uring_submit(&impl_->ring);
        if (rc < 0)
        {
            return fail("io_uring_submit", -rc);
        }
        int res = 0;
        if (!impl_->wait(res))
        {
            return fail("io_uring_wait_cqe", impl_->err);
        }
        if (res < 0)
        {
            return fail("io_uring read", -res);
        }
        if (res == 0)
        {
            // Short read at EOF. Sparse table files rely on the zero fill.
            std::memset(cursor, 0, left);
            return {};
        }
        cursor += res;
        offset += static_cast<uint64_t>(res);
        left -= static_cast<uint32_t>(res);
    }
    return {};
}

popo::base::Result<void> IoRing::write_at(int fd, const void * buf, uint32_t len, uint64_t offset)
{
    auto * cursor = static_cast<const char *>(buf);
    uint32_t left = len;
    while (left > 0)
    {
        io_uring_sqe * sqe = io_uring_get_sqe(&impl_->ring);
        if (sqe == nullptr)
        {
            return fail("io_uring_get_sqe", EAGAIN);
        }
        io_uring_prep_write(sqe, fd, cursor, left, offset);
        int rc = io_uring_submit(&impl_->ring);
        if (rc < 0)
        {
            return fail("io_uring_submit", -rc);
        }
        int res = 0;
        if (!impl_->wait(res))
        {
            return fail("io_uring_wait_cqe", impl_->err);
        }
        if (res < 0)
        {
            return fail("io_uring write", -res);
        }
        if (res == 0)
        {
            return fail("io_uring write", EIO);
        }
        cursor += res;
        offset += static_cast<uint64_t>(res);
        left -= static_cast<uint32_t>(res);
    }
    return {};
}

popo::base::Result<void> IoRing::fsync(int fd)
{
    return sync_writes(fd, nullptr, 0, false);
}

popo::base::Result<void> IoRing::write_batch(int fd, const Write * writes, size_t count)
{
    size_t base = 0;
    while (base < count)
    {
        size_t span = std::min<size_t>(count - base, QUEUE_DEPTH);
        unsigned queued = 0;
        for (size_t i = 0; i < span; ++i)
        {
            const Write & write = writes[base + i];
            if (write.len == 0)
            {
                continue;
            }
            io_uring_sqe * sqe = io_uring_get_sqe(&impl_->ring);
            if (sqe == nullptr)
            {
                return fail("io_uring_get_sqe", EAGAIN);
            }
            io_uring_prep_write(sqe, fd, write.data, write.len, write.offset);
            io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(i));
            ++queued;
        }
        int rc = io_uring_submit(&impl_->ring);
        if (rc < 0)
        {
            return fail("io_uring_submit", -rc);
        }

        // Reap the whole batch even after a failure: leaving completions in the
        // CQ would desynchronise every later wait.
        int first_err = 0;
        std::vector<std::pair<size_t, uint32_t>> shorts;
        for (unsigned done = 0; done < queued; ++done)
        {
            io_uring_cqe * cqe = nullptr;
            rc = io_uring_wait_cqe(&impl_->ring, &cqe);
            if (rc < 0)
            {
                return fail("io_uring_wait_cqe", -rc);
            }
            int res = cqe->res;
            size_t index = static_cast<size_t>(io_uring_cqe_get_data64(cqe));
            io_uring_cqe_seen(&impl_->ring, cqe);
            if (res < 0)
            {
                if (first_err == 0)
                {
                    first_err = -res;
                }
                continue;
            }
            if (static_cast<uint32_t>(res) != writes[base + index].len)
            {
                shorts.emplace_back(index, static_cast<uint32_t>(res));
            }
        }
        if (first_err != 0)
        {
            return fail("io_uring write", first_err);
        }
        // The ring is idle again, so the tails can be written one by one.
        for (const auto & [index, done] : shorts)
        {
            const Write & write = writes[base + index];
            if (done == 0)
            {
                return fail("io_uring write", EIO);
            }
            auto rest = write_at(fd, static_cast<const char *>(write.data) + done, write.len - done, write.offset + done);
            if (!rest)
            {
                return rest;
            }
        }
        base += span;
    }
    return {};
}

popo::base::Result<void> IoRing::sync_writes(int fd, const Write * writes, size_t count, bool data_sync)
{
    auto written = write_batch(fd, writes, count);
    if (!written)
    {
        return written;
    }
    return fsync_impl(fd, data_sync);
}

popo::base::Result<void> IoRing::write_and_fsync(int fd, const void * buf, uint32_t len, uint64_t offset, bool data_sync)
{
    auto * cursor = static_cast<const char *>(buf);
    uint32_t left = len;
    while (left > 0)
    {
        if (io_uring_sq_space_left(&impl_->ring) < 2)
        {
            return fail("io_uring_get_sqe", EAGAIN);
        }
        io_uring_sqe * write_sqe = io_uring_get_sqe(&impl_->ring);
        io_uring_sqe * sync_sqe = io_uring_get_sqe(&impl_->ring);
        io_uring_prep_write(write_sqe, fd, cursor, left, offset);
        io_uring_prep_fsync(sync_sqe, fd, data_sync ? IORING_FSYNC_DATASYNC : 0);
        io_uring_sqe_set_data64(write_sqe, 0);
        io_uring_sqe_set_data64(sync_sqe, 1);
        // The fsync must observe the write, otherwise the barrier is void.
        write_sqe->flags |= IOSQE_IO_LINK;

        int rc = io_uring_submit(&impl_->ring);
        if (rc < 0)
        {
            return fail("io_uring_submit", -rc);
        }
        int write_res = 0;
        int sync_res = 0;
        for (int i = 0; i < 2; ++i)
        {
            io_uring_cqe * cqe = nullptr;
            rc = io_uring_wait_cqe(&impl_->ring, &cqe);
            if (rc < 0)
            {
                return fail("io_uring_wait_cqe", -rc);
            }
            int res = cqe->res;
            if (io_uring_cqe_get_data64(cqe) == 0)
            {
                write_res = res;
            }
            else
            {
                sync_res = res;
            }
            io_uring_cqe_seen(&impl_->ring, cqe);
        }
        // A failed write cancels the linked fsync, so report the write first.
        if (write_res < 0)
        {
            return fail("io_uring write", -write_res);
        }
        if (sync_res < 0)
        {
            return fail(data_sync ? "io_uring fdatasync" : "io_uring fsync", -sync_res);
        }
        if (write_res == 0)
        {
            return fail("io_uring write", EIO);
        }
        cursor += write_res;
        offset += static_cast<uint64_t>(write_res);
        left -= static_cast<uint32_t>(write_res);
    }
    return {};
}

popo::base::Result<void> IoRing::fsync_impl(int fd, bool data_sync)
{
    io_uring_sqe * sqe = io_uring_get_sqe(&impl_->ring);
    if (sqe == nullptr)
    {
        return fail("io_uring_get_sqe", EAGAIN);
    }
    io_uring_prep_fsync(sqe, fd, data_sync ? IORING_FSYNC_DATASYNC : 0);
    int rc = io_uring_submit(&impl_->ring);
    if (rc < 0)
    {
        return fail("io_uring_submit", -rc);
    }
    int res = 0;
    if (!impl_->wait(res))
    {
        return fail("io_uring_wait_cqe", impl_->err);
    }
    if (res < 0)
    {
        return fail(data_sync ? "io_uring fdatasync" : "io_uring fsync", -res);
    }
    return {};
}

} // namespace popo::storage
