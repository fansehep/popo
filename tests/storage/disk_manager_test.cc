#include <cstring>
#include <filesystem>
#include <random>
#include <gtest/gtest.h>
#include "src/storage/common.h"
#include "src/storage/disk_manager.h"

using namespace neon::storage;
namespace fs = std::filesystem;

class DiskManagerTest : public ::testing::Test
{
protected:
    std::string test_dir;
    std::unique_ptr<DiskManager> dm;

    void SetUp() override
    {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/neon_dm_test_" + std::to_string(rd());
        fs::create_directories(test_dir);
        dm = std::make_unique<DiskManager>(test_dir);
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
    dm->create_table_file("test_table").value();
    EXPECT_TRUE(dm->table_file_exists("test_table"));
}

TEST_F(DiskManagerTest, AllocateAndReadWritePage)
{
    dm->create_table_file("t1").value();
    page_id_t pid = dm->allocate_page("t1").value();
    EXPECT_NE(pid, INVALID_PAGE_ID);

    char write_buf[PAGE_SIZE];
    std::memset(write_buf, 0, PAGE_SIZE);
    std::string payload = "hello disk manager";
    std::memcpy(write_buf, payload.data(), payload.size());
    dm->write_page("t1", pid, write_buf).value();

    char read_buf[PAGE_SIZE];
    std::memset(read_buf, 0, PAGE_SIZE);
    dm->read_page("t1", pid, read_buf).value();

    EXPECT_EQ(std::memcmp(write_buf, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, MultiplePages)
{
    dm->create_table_file("t1").value();
    constexpr int NUM_PAGES = 5;
    page_id_t pids[NUM_PAGES];
    char write_bufs[NUM_PAGES][PAGE_SIZE];

    for (int i = 0; i < NUM_PAGES; ++i)
    {
        pids[i] = dm->allocate_page("t1").value();
        std::memset(write_bufs[i], 0, PAGE_SIZE);
        std::string payload = "page_data_" + std::to_string(i);
        std::memcpy(write_bufs[i], payload.data(), payload.size());
        dm->write_page("t1", pids[i], write_bufs[i]).value();
    }

    for (int i = 0; i < NUM_PAGES; ++i)
    {
        char read_buf[PAGE_SIZE];
        dm->read_page("t1", pids[i], read_buf).value();
        EXPECT_EQ(std::memcmp(write_bufs[i], read_buf, PAGE_SIZE), 0);
    }
}

TEST_F(DiskManagerTest, PageCount)
{
    dm->create_table_file("t1").value();
    EXPECT_EQ(dm->page_count("t1"), 0u);

    dm->allocate_page("t1").value();
    EXPECT_EQ(dm->page_count("t1"), 1u);

    dm->allocate_page("t1").value();
    dm->allocate_page("t1").value();
    EXPECT_EQ(dm->page_count("t1"), 3u);
}

TEST_F(DiskManagerTest, RemoveTableFile)
{
    dm->create_table_file("t1").value();
    EXPECT_TRUE(dm->table_file_exists("t1"));
    dm->remove_table_file("t1");
    EXPECT_FALSE(dm->table_file_exists("t1"));
}

TEST_F(DiskManagerTest, RewriteKeepsLatestAndReopens)
{
    dm->create_table_file("t1").value();
    page_id_t pid = dm->allocate_page("t1").value();

    char first[PAGE_SIZE];
    char second[PAGE_SIZE];
    std::memset(first, 0, PAGE_SIZE);
    std::memset(second, 0, PAGE_SIZE);
    std::memcpy(first, "v1", 2);
    std::memcpy(second, "v2", 2);
    dm->write_page("t1", pid, first).value();
    dm->write_page("t1", pid, second).value();

    char read_buf[PAGE_SIZE];
    dm->read_page("t1", pid, read_buf).value();
    EXPECT_EQ(std::memcmp(second, read_buf, PAGE_SIZE), 0);

    dm.reset();
    dm = std::make_unique<DiskManager>(test_dir);
    std::memset(read_buf, 0, PAGE_SIZE);
    dm->read_page("t1", pid, read_buf).value();
    EXPECT_EQ(std::memcmp(second, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, SingleTransactionAbortAndCommit)
{
    dm->create_table_file("t1").value();
    page_id_t pid = dm->allocate_page("t1").value();

    char committed[PAGE_SIZE];
    char pending[PAGE_SIZE];
    std::memset(committed, 0, PAGE_SIZE);
    std::memset(pending, 0, PAGE_SIZE);
    std::memcpy(committed, "committed", 9);
    std::memcpy(pending, "pending", 7);
    dm->write_page("t1", pid, committed).value();

    EXPECT_TRUE(dm->begin_txn());
    dm->write_page("t1", pid, pending).value();
    char during[PAGE_SIZE];
    dm->read_page("t1", pid, during).value();
    EXPECT_EQ(std::memcmp(pending, during, PAGE_SIZE), 0);
    dm->abort_txn();

    char after_abort[PAGE_SIZE];
    dm->read_page("t1", pid, after_abort).value();
    EXPECT_EQ(std::memcmp(committed, after_abort, PAGE_SIZE), 0);

    EXPECT_TRUE(dm->begin_txn());
    dm->write_page("t1", pid, pending).value();
    dm->commit_txn();
    char after_commit[PAGE_SIZE];
    dm->read_page("t1", pid, after_commit).value();
    EXPECT_EQ(std::memcmp(pending, after_commit, PAGE_SIZE), 0);

    dm.reset();
    dm = std::make_unique<DiskManager>(test_dir);
    std::memset(after_commit, 0, PAGE_SIZE);
    dm->read_page("t1", pid, after_commit).value();
    EXPECT_EQ(std::memcmp(pending, after_commit, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, GcReusesBlocks)
{
    dm->create_table_file("t1").value();
    page_id_t pid = dm->allocate_page("t1").value();

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
        dm->write_page("t1", pid, buf).value();
    }

    auto size_after = fs::file_size(test_dir + "/t1.ndb");
    EXPECT_LT(size_after, static_cast<uintmax_t>(4) * BLOCK_SIZE);

    char read_buf[PAGE_SIZE];
    dm->read_page("t1", pid, read_buf).value();
    EXPECT_EQ(std::memcmp(buf, read_buf, PAGE_SIZE), 0);
}
