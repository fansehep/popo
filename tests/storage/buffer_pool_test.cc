#include <cstring>
#include <filesystem>
#include <random>
#include <gtest/gtest.h>
#include "src/storage/buffer_pool.h"
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"
#include "src/storage/page.h"

using namespace popo::storage;
namespace fs = std::filesystem;

class BufferPoolTest : public ::testing::Test
{
protected:
    std::string test_dir;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPoolManager> bpm;

    void SetUp() override
    {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/popo_bp_test_" + std::to_string(rd());
        fs::create_directories(test_dir);
        dm = std::make_unique<DiskManager>(test_dir);
        dm->create_table_file("t1");
        bpm = std::make_unique<BufferPoolManager>(10, dm.get());
    }

    void TearDown() override
    {
        bpm.reset();
        dm.reset();
        fs::remove_all(test_dir);
    }
};

TEST_F(BufferPoolTest, FetchNewPage)
{
    page_id_t pid;
    auto guard = bpm->new_page("t1", pid);
    ASSERT_TRUE(guard.is_valid());
    EXPECT_NE(pid, INVALID_PAGE_ID);

    // Write some data
    std::memset(guard.data(), 0, PAGE_SIZE);
    std::string payload = "buffer pool test";
    std::memcpy(guard.data(), payload.data(), payload.size());
    guard.mark_dirty();

    page_id_t saved_pid = pid;
    // Drop the guard, then fetch same page
    guard = PageGuard{};

    auto guard2 = bpm->fetch_page("t1", saved_pid);
    ASSERT_TRUE(guard2.is_valid());
    EXPECT_EQ(std::string(guard2.data(), payload.size()), payload);
}

TEST_F(BufferPoolTest, FetchFromDisk)
{
    page_id_t pid;
    {
        auto guard = bpm->new_page("t1", pid);
        ASSERT_TRUE(guard.is_valid());
        std::string payload = "disk fetch test";
        std::memset(guard.data(), 0, PAGE_SIZE);
        std::memcpy(guard.data(), payload.data(), payload.size());
        guard.mark_dirty();
    }

    // Flush to disk
    bpm->flush_page("t1", pid);

    // Create a new buffer pool to force reading from disk
    bpm = std::make_unique<BufferPoolManager>(10, dm.get());
    auto guard = bpm->fetch_page("t1", pid);
    ASSERT_TRUE(guard.is_valid());
    std::string expected = "disk fetch test";
    EXPECT_EQ(std::string(guard.data(), expected.size()), expected);
}

TEST_F(BufferPoolTest, EvictionPolicy)
{
    // Small pool: 3 frames
    bpm = std::make_unique<BufferPoolManager>(3, dm.get());

    page_id_t pids[4];
    for (int i = 0; i < 3; ++i)
    {
        auto guard = bpm->new_page("t1", pids[i]);
        ASSERT_TRUE(guard.is_valid());
        std::string payload = "page_" + std::to_string(i);
        std::memset(guard.data(), 0, PAGE_SIZE);
        std::memcpy(guard.data(), payload.data(), payload.size());
        guard.mark_dirty();
    }

    // 4th page should trigger eviction
    auto guard = bpm->new_page("t1", pids[3]);
    ASSERT_TRUE(guard.is_valid());
    std::string payload = "page_3";
    std::memset(guard.data(), 0, PAGE_SIZE);
    std::memcpy(guard.data(), payload.data(), payload.size());
    guard.mark_dirty();
    guard = PageGuard{};

    // Should still be able to fetch any page (evicted pages read from disk)
    for (int i = 0; i < 4; ++i)
    {
        auto g = bpm->fetch_page("t1", pids[i]);
        ASSERT_TRUE(g.is_valid()) << "Failed to fetch page " << i;
        std::string expected = "page_" + std::to_string(i);
        EXPECT_EQ(std::string(g.data(), expected.size()), expected);
        // Must unpin before fetching next (pool size is 3)
    }
}

TEST_F(BufferPoolTest, DirtyPageFlush)
{
    page_id_t pid;
    {
        auto guard = bpm->new_page("t1", pid);
        ASSERT_TRUE(guard.is_valid());
        std::string payload = "dirty page data";
        std::memset(guard.data(), 0, PAGE_SIZE);
        std::memcpy(guard.data(), payload.data(), payload.size());
        guard.mark_dirty();
    }

    // Flush all to ensure dirty page is written
    bpm->flush_all();

    // Read directly from disk to verify
    char buf[PAGE_SIZE];
    dm->read_page("t1", pid, buf);
    std::string expected = "dirty page data";
    EXPECT_EQ(std::string(buf, expected.size()), expected);
}

TEST_F(BufferPoolTest, PageGuardRAII)
{
    page_id_t pid;
    {
        auto guard = bpm->new_page("t1", pid);
        ASSERT_TRUE(guard.is_valid());
        guard.mark_dirty();
        // guard goes out of scope here -> should unpin
    }
    // After unpin, page should be fetchable again
    auto guard2 = bpm->fetch_page("t1", pid);
    ASSERT_TRUE(guard2.is_valid());
}
