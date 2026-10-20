#include <filesystem>
#include <random>
#include <gtest/gtest.h>
#include "src/storage/common.h"
#include "src/storage/redo_log.h"

using namespace popo::storage;
namespace fs = std::filesystem;

class RedoLogTest : public ::testing::Test
{
protected:
    std::string test_dir;
    std::string log_path;

    void SetUp() override
    {
        std::random_device rd;
        test_dir = fs::temp_directory_path().string() + "/popo_rl_test_" + std::to_string(rd());
        fs::create_directories(test_dir);
        log_path = test_dir + "/redo.log";
    }

    void TearDown() override { fs::remove_all(test_dir); }

    LogRecord make_insert_record(txn_id_t txn_id, const std::string & table, page_id_t pid, slot_id_t sid, const std::string & data)
    {
        LogRecord rec;
        rec.txn_id = txn_id;
        rec.type = LogType::INSERT;
        rec.table_name = table;
        rec.page_id = pid;
        rec.slot_id = sid;
        rec.new_data.assign(data.begin(), data.end());
        return rec;
    }
};

TEST_F(RedoLogTest, AppendAndFlush)
{
    RedoLogManager mgr(log_path);
    EXPECT_EQ(mgr.flushed_lsn(), 0u);

    lsn_t lsn1 = mgr.append(make_insert_record(1, "t1", 1, 0, "data1"));
    lsn_t lsn2 = mgr.append(make_insert_record(1, "t1", 1, 1, "data2"));
    EXPECT_GT(lsn2, lsn1);

    mgr.flush();
    EXPECT_GE(mgr.flushed_lsn(), lsn2);
}

TEST_F(RedoLogTest, ReadAllRecords)
{
    {
        RedoLogManager mgr(log_path);
        mgr.append(make_insert_record(1, "t1", 1, 0, "record_a"));
        mgr.append(make_insert_record(2, "t1", 1, 1, "record_b"));
        mgr.append(make_insert_record(3, "t2", 2, 0, "record_c"));
        mgr.flush();
    }

    RedoLogManager mgr2(log_path);
    auto records = mgr2.read_all_records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].txn_id, 1u);
    EXPECT_EQ(std::string(records[0].new_data.begin(), records[0].new_data.end()), "record_a");
    EXPECT_EQ(records[1].txn_id, 2u);
    EXPECT_EQ(std::string(records[1].new_data.begin(), records[1].new_data.end()), "record_b");
    EXPECT_EQ(records[2].table_name, "t2");
    EXPECT_EQ(std::string(records[2].new_data.begin(), records[2].new_data.end()), "record_c");
}

TEST_F(RedoLogTest, RecoveryLSN)
{
    lsn_t last_lsn;
    {
        RedoLogManager mgr(log_path);
        mgr.append(make_insert_record(1, "t1", 1, 0, "data1"));
        mgr.append(make_insert_record(1, "t1", 1, 1, "data2"));
        last_lsn = mgr.current_lsn();
        mgr.flush();
    }

    // New manager on the same file should continue LSN numbering
    RedoLogManager mgr2(log_path);
    lsn_t new_lsn = mgr2.append(make_insert_record(2, "t1", 1, 2, "data3"));
    EXPECT_GE(new_lsn, last_lsn);
}

TEST_F(RedoLogTest, Truncate)
{
    RedoLogManager mgr(log_path);
    mgr.append(make_insert_record(1, "t1", 1, 0, "data1"));
    mgr.append(make_insert_record(1, "t1", 1, 1, "data2"));
    mgr.flush();

    auto records_before = mgr.read_all_records();
    EXPECT_EQ(records_before.size(), 2u);

    mgr.truncate();
    auto records_after = mgr.read_all_records();
    EXPECT_TRUE(records_after.empty());
}
