#include <gtest/gtest.h>
#include "src/storage/disk_manager.h"
#include "src/storage/common.h"
#include <filesystem>
#include <cstring>
#include <random>

using namespace neon::storage;
namespace fs = std::filesystem;

class DiskManagerTest : public ::testing::Test {
protected:
    std::string test_dir;
    std::unique_ptr<DiskManager> dm;

    void SetUp() override {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/neon_dm_test_" + std::to_string(rd());
        fs::create_directories(test_dir);
        dm = std::make_unique<DiskManager>(test_dir);
    }

    void TearDown() override {
        dm.reset();
        fs::remove_all(test_dir);
    }
};

TEST_F(DiskManagerTest, CreateAndCheckTableFile) {
    EXPECT_FALSE(dm->table_file_exists("test_table"));
    dm->create_table_file("test_table");
    EXPECT_TRUE(dm->table_file_exists("test_table"));
}

TEST_F(DiskManagerTest, AllocateAndReadWritePage) {
    dm->create_table_file("t1");
    page_id_t pid = dm->allocate_page("t1");
    EXPECT_NE(pid, INVALID_PAGE_ID);

    char write_buf[PAGE_SIZE];
    std::memset(write_buf, 0, PAGE_SIZE);
    std::string payload = "hello disk manager";
    std::memcpy(write_buf, payload.data(), payload.size());
    dm->write_page("t1", pid, write_buf);

    char read_buf[PAGE_SIZE];
    std::memset(read_buf, 0, PAGE_SIZE);
    dm->read_page("t1", pid, read_buf);

    EXPECT_EQ(std::memcmp(write_buf, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, MultiplePages) {
    dm->create_table_file("t1");
    constexpr int NUM_PAGES = 5;
    page_id_t pids[NUM_PAGES];
    char write_bufs[NUM_PAGES][PAGE_SIZE];

    for (int i = 0; i < NUM_PAGES; ++i) {
        pids[i] = dm->allocate_page("t1");
        std::memset(write_bufs[i], 0, PAGE_SIZE);
        std::string payload = "page_data_" + std::to_string(i);
        std::memcpy(write_bufs[i], payload.data(), payload.size());
        dm->write_page("t1", pids[i], write_bufs[i]);
    }

    for (int i = 0; i < NUM_PAGES; ++i) {
        char read_buf[PAGE_SIZE];
        dm->read_page("t1", pids[i], read_buf);
        EXPECT_EQ(std::memcmp(write_bufs[i], read_buf, PAGE_SIZE), 0);
    }
}

TEST_F(DiskManagerTest, PageCount) {
    dm->create_table_file("t1");
    EXPECT_EQ(dm->page_count("t1"), 0u);

    dm->allocate_page("t1");
    EXPECT_EQ(dm->page_count("t1"), 1u);

    dm->allocate_page("t1");
    dm->allocate_page("t1");
    EXPECT_EQ(dm->page_count("t1"), 3u);
}

TEST_F(DiskManagerTest, RemoveTableFile) {
    dm->create_table_file("t1");
    EXPECT_TRUE(dm->table_file_exists("t1"));
    dm->remove_table_file("t1");
    EXPECT_FALSE(dm->table_file_exists("t1"));
}
