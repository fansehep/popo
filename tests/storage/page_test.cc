#include <gtest/gtest.h>
#include "src/storage/page.h"

using namespace neon::storage;

class PageTest : public ::testing::Test
{
protected:
    DataPage page{};

    void SetUp() override { page.init(1); }
};

TEST_F(PageTest, PageInit)
{
    auto * hdr = page.header();
    EXPECT_EQ(hdr->page_id, 1u);
    EXPECT_EQ(hdr->page_type, static_cast<uint16_t>(PageType::DATA_PAGE));
    EXPECT_EQ(hdr->slot_count, 0);
    EXPECT_EQ(page.free_space(), PAGE_SIZE - PAGE_HEADER_SIZE);
}

TEST_F(PageTest, InsertAndGetTuple)
{
    std::string data = "hello world";
    auto slot = page.insert_tuple(data.data(), static_cast<uint16_t>(data.size()));
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(slot.value(), 0);

    auto [ptr, len] = page.get_tuple(slot.value());
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(len, data.size());
    EXPECT_EQ(std::string(ptr, len), data);
}

TEST_F(PageTest, InsertMultipleTuples)
{
    std::vector<std::string> tuples = {"aaa", "bbb", "ccc", "ddd"};
    std::vector<slot_id_t> slots;
    for (auto & t : tuples)
    {
        auto s = page.insert_tuple(t.data(), static_cast<uint16_t>(t.size()));
        ASSERT_TRUE(s.has_value());
        slots.push_back(s.value());
    }
    EXPECT_EQ(page.slot_count(), 4);

    for (size_t i = 0; i < tuples.size(); ++i)
    {
        auto [ptr, len] = page.get_tuple(slots[i]);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(std::string(ptr, len), tuples[i]);
    }
}

TEST_F(PageTest, DeleteTuple)
{
    std::string data = "delete_me";
    auto slot = page.insert_tuple(data.data(), static_cast<uint16_t>(data.size()));
    ASSERT_TRUE(slot.has_value());

    EXPECT_TRUE(page.delete_tuple(slot.value()));
    auto [ptr, len] = page.get_tuple(slot.value());
    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(len, 0);
}

TEST_F(PageTest, UpdateTupleInPlace)
{
    std::string data = "original";
    auto slot = page.insert_tuple(data.data(), static_cast<uint16_t>(data.size()));
    ASSERT_TRUE(slot.has_value());

    std::string updated = "replaced"; // same size
    auto new_slot = page.update_tuple(slot.value(), updated.data(), static_cast<uint16_t>(updated.size()));
    ASSERT_TRUE(new_slot.has_value());
    EXPECT_EQ(new_slot.value(), slot.value()); // in-place

    auto [ptr, len] = page.get_tuple(new_slot.value());
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(std::string(ptr, len), updated);
}

TEST_F(PageTest, UpdateTupleRelocate)
{
    std::string small = "hi";
    auto slot = page.insert_tuple(small.data(), static_cast<uint16_t>(small.size()));
    ASSERT_TRUE(slot.has_value());

    // Data larger than old slot — should return nullopt (caller handles re-insert)
    std::string large = "this is a much larger string that won't fit in the old slot";
    auto new_slot = page.update_tuple(slot.value(), large.data(), static_cast<uint16_t>(large.size()));
    EXPECT_FALSE(new_slot.has_value()) << "Relocation is no longer supported in-page; caller should delete+reinsert";

    // Old slot should still be valid (update_tuple didn't modify it on failure)
    auto [old_ptr, old_len] = page.get_tuple(slot.value());
    ASSERT_NE(old_ptr, nullptr);
    EXPECT_EQ(std::string(old_ptr, old_len), small);
}

TEST_F(PageTest, PageFull)
{
    // Fill page with large tuples until it's full
    std::string big_data(500, 'x');
    int count = 0;
    while (true)
    {
        auto slot = page.insert_tuple(big_data.data(), static_cast<uint16_t>(big_data.size()));
        if (!slot.has_value())
            break;
        count++;
    }
    EXPECT_GT(count, 0);
    // Verify no more can be inserted
    auto slot = page.insert_tuple(big_data.data(), static_cast<uint16_t>(big_data.size()));
    EXPECT_FALSE(slot.has_value());
}

TEST_F(PageTest, FreeSpaceTracking)
{
    uint16_t initial_free = page.free_space();
    EXPECT_EQ(initial_free, PAGE_SIZE - PAGE_HEADER_SIZE);

    std::string data = "test_data";
    auto slot = page.insert_tuple(data.data(), static_cast<uint16_t>(data.size()));
    ASSERT_TRUE(slot.has_value());

    uint16_t expected_free = initial_free - SLOT_ENTRY_SIZE - static_cast<uint16_t>(data.size());
    EXPECT_EQ(page.free_space(), expected_free);
}
