#include <cstring>
#include <filesystem>
#include <random>
#include <gtest/gtest.h>
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"

using namespace popo::storage;
namespace fs = std::filesystem;

class DiskManagerTest : public ::testing::Test
{
protected:
    std::string test_dir;
    std::unique_ptr<DiskManager> dm;

    void SetUp() override
    {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/popo_dm_test_" + std::to_string(rd());
        fs::create_directories(test_dir);
        dm = std::make_unique<DiskManager>(test_dir);
        ASSERT_TRUE(dm->status());
    }

    void TearDown() override
    {
        dm.reset();
        fs::remove_all(test_dir);
    }
};

TEST_F(DiskManagerTest, CreateAndCheckTableFile)
{
    EXPECT_FALSE(dm->table_file_exists("test_table"));
    ASSERT_TRUE(dm->create_table_file("test_table"));
    EXPECT_TRUE(dm->table_file_exists("test_table"));
}

TEST_F(DiskManagerTest, AllocateAndReadWritePage)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    auto allocated = dm->allocate_page("t1");
    ASSERT_TRUE(allocated);
    page_id_t pid = *allocated;
    EXPECT_NE(pid, INVALID_PAGE_ID);

    char write_buf[PAGE_SIZE];
    std::memset(write_buf, 0, PAGE_SIZE);
    std::string payload = "hello disk manager";
    std::memcpy(write_buf, payload.data(), payload.size());
    ASSERT_TRUE(dm->write_page("t1", pid, write_buf));

    char read_buf[PAGE_SIZE];
    std::memset(read_buf, 0, PAGE_SIZE);
    ASSERT_TRUE(dm->read_page("t1", pid, read_buf));

    EXPECT_EQ(std::memcmp(write_buf, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, MultiplePages)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    constexpr int NUM_PAGES = 5;
    page_id_t pids[NUM_PAGES];
    char write_bufs[NUM_PAGES][PAGE_SIZE];

    for (int i = 0; i < NUM_PAGES; ++i)
    {
        auto allocated = dm->allocate_page("t1");
        ASSERT_TRUE(allocated);
        pids[i] = *allocated;
        std::memset(write_bufs[i], 0, PAGE_SIZE);
        std::string payload = "page_data_" + std::to_string(i);
        std::memcpy(write_bufs[i], payload.data(), payload.size());
        ASSERT_TRUE(dm->write_page("t1", pids[i], write_bufs[i]));
    }

    for (int i = 0; i < NUM_PAGES; ++i)
    {
        char read_buf[PAGE_SIZE];
        ASSERT_TRUE(dm->read_page("t1", pids[i], read_buf));
        EXPECT_EQ(std::memcmp(write_bufs[i], read_buf, PAGE_SIZE), 0);
    }
}

TEST_F(DiskManagerTest, PageCount)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    EXPECT_EQ(dm->page_count("t1"), 0u);

    ASSERT_TRUE(dm->allocate_page("t1"));
    EXPECT_EQ(dm->page_count("t1"), 1u);

    ASSERT_TRUE(dm->allocate_page("t1"));
    ASSERT_TRUE(dm->allocate_page("t1"));
    EXPECT_EQ(dm->page_count("t1"), 3u);
}

TEST_F(DiskManagerTest, RemoveTableFile)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    EXPECT_TRUE(dm->table_file_exists("t1"));
    dm->remove_table_file("t1");
    EXPECT_FALSE(dm->table_file_exists("t1"));
}

TEST_F(DiskManagerTest, RewriteKeepsLatestAndReopens)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    auto allocated = dm->allocate_page("t1");
    ASSERT_TRUE(allocated);
    page_id_t pid = *allocated;

    char first[PAGE_SIZE];
    char second[PAGE_SIZE];
    std::memset(first, 0, PAGE_SIZE);
    std::memset(second, 0, PAGE_SIZE);
    std::memcpy(first, "v1", 2);
    std::memcpy(second, "v2", 2);
    ASSERT_TRUE(dm->write_page("t1", pid, first));
    ASSERT_TRUE(dm->write_page("t1", pid, second));

    char read_buf[PAGE_SIZE];
    ASSERT_TRUE(dm->read_page("t1", pid, read_buf));
    EXPECT_EQ(std::memcmp(second, read_buf, PAGE_SIZE), 0);

    dm.reset();
    dm = std::make_unique<DiskManager>(test_dir);
    ASSERT_TRUE(dm->status());
    std::memset(read_buf, 0, PAGE_SIZE);
    ASSERT_TRUE(dm->read_page("t1", pid, read_buf));
    EXPECT_EQ(std::memcmp(second, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, SingleTransactionAbortAndCommit)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    auto allocated = dm->allocate_page("t1");
    ASSERT_TRUE(allocated);
    page_id_t pid = *allocated;

    char committed[PAGE_SIZE];
    char pending[PAGE_SIZE];
    std::memset(committed, 0, PAGE_SIZE);
    std::memset(pending, 0, PAGE_SIZE);
    std::memcpy(committed, "committed", 9);
    std::memcpy(pending, "pending", 7);
    ASSERT_TRUE(dm->write_page("t1", pid, committed));

    EXPECT_TRUE(dm->begin_txn());
    ASSERT_TRUE(dm->write_page("t1", pid, pending));
    char during[PAGE_SIZE];
    ASSERT_TRUE(dm->read_page("t1", pid, during));
    EXPECT_EQ(std::memcmp(pending, during, PAGE_SIZE), 0);
    dm->abort_txn();

    char after_abort[PAGE_SIZE];
    ASSERT_TRUE(dm->read_page("t1", pid, after_abort));
    EXPECT_EQ(std::memcmp(committed, after_abort, PAGE_SIZE), 0);

    EXPECT_TRUE(dm->begin_txn());
    ASSERT_TRUE(dm->write_page("t1", pid, pending));
    ASSERT_TRUE(dm->commit_txn());
    char after_commit[PAGE_SIZE];
    ASSERT_TRUE(dm->read_page("t1", pid, after_commit));
    EXPECT_EQ(std::memcmp(pending, after_commit, PAGE_SIZE), 0);

    dm.reset();
    dm = std::make_unique<DiskManager>(test_dir);
    ASSERT_TRUE(dm->status());
    std::memset(after_commit, 0, PAGE_SIZE);
    ASSERT_TRUE(dm->read_page("t1", pid, after_commit));
    EXPECT_EQ(std::memcmp(pending, after_commit, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, GcReusesBlocks)
{
    ASSERT_TRUE(dm->create_table_file("t1"));
    auto allocated = dm->allocate_page("t1");
    ASSERT_TRUE(allocated);
    page_id_t pid = *allocated;

    std::mt19937 rng(1);
    char buf[PAGE_SIZE];
    constexpr int kWrites = 80;
    for (int i = 0; i < kWrites; ++i)
    {
        for (int j = 0; j < PAGE_SIZE; ++j)
        {
            buf[j] = static_cast<char>(rng());
        }
        buf[0] = static_cast<char>(i);
        ASSERT_TRUE(dm->write_page("t1", pid, buf));
    }

    auto size_after = fs::file_size(test_dir + "/t1.ndb");
    EXPECT_LT(size_after, static_cast<uintmax_t>(4) * BLOCK_SIZE);

    char read_buf[PAGE_SIZE];
    ASSERT_TRUE(dm->read_page("t1", pid, read_buf));
    EXPECT_EQ(std::memcmp(buf, read_buf, PAGE_SIZE), 0);
}
