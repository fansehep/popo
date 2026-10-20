#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include "src/storage/btree_page.h"
#include "src/storage/page.h"

using namespace popo::storage;

namespace
{

// Helper: create a simple int32 comparator for internal page keys
int int32_cmp(const char * a, uint16_t al, const char * b, uint16_t bl)
{
    (void)al;
    (void)bl;
    int32_t va = *reinterpret_cast<const int32_t *>(a);
    int32_t vb = *reinterpret_cast<const int32_t *>(b);
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

// Helper: serialize int32 into vector
std::vector<char> int32_key(int32_t v)
{
    std::vector<char> k(sizeof(int32_t));
    std::memcpy(k.data(), &v, sizeof(int32_t));
    return k;
}

} // anonymous namespace

// ============================================================================
// BTreeInternalPage Tests
// ============================================================================

TEST(BTreeInternalPageTest, Init)
{
    BTreeInternalPage page;
    page.init(42, 0);

    EXPECT_EQ(page.header()->page_id, 42u);
    EXPECT_EQ(static_cast<PageType>(page.header()->page_type), PageType::BPTREE_INTERNAL_PAGE);
    EXPECT_EQ(page.level(), 0u);
    EXPECT_EQ(page.key_count(), 0u);
    EXPECT_EQ(page.rightmost_child(), INVALID_PAGE_ID);
}

TEST(BTreeInternalPageTest, InsertSingleKey)
{
    BTreeInternalPage page;
    page.init(1, 0);

    auto key = int32_key(100);
    bool ok = page.insert(key.data(), sizeof(int32_t), 10, int32_cmp);
    EXPECT_TRUE(ok);
    EXPECT_EQ(page.key_count(), 1u);

    // rightmost_child was INVALID, now entry[0].child = INVALID, rightmost = 10
    EXPECT_EQ(page.child_at(0), INVALID_PAGE_ID);
    EXPECT_EQ(page.child_at(1), 10u);

    // Find child: search key 50 (< 100) -> child_at(0) = leftmost = INVALID
    auto k50 = int32_key(50);
    EXPECT_EQ(page.find_child(k50.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);

    // Find child: search key 150 (>= 100) -> child_at(1) = rightmost = 10
    auto k150 = int32_key(150);
    EXPECT_EQ(page.find_child(k150.data(), sizeof(int32_t), int32_cmp), 10u);
}

TEST(BTreeInternalPageTest, InsertMultipleKeysSorted)
{
    BTreeInternalPage page;
    page.init(1, 0);

    // Insert keys in ascending order: 10, 20, 30
    for (int32_t v : {10, 20, 30})
    {
        auto k = int32_key(v);
        bool ok = page.insert(k.data(), sizeof(int32_t), v * 10, int32_cmp);
        EXPECT_TRUE(ok);
    }
    EXPECT_EQ(page.key_count(), 3u);

    // child_at(0) = entry[0].child (for key 10) = still INVALID (first entry ever)
    // Wait, let me trace more carefully.
    // Initial: key_count=0, rightmost=INVALID
    // Insert key=10, child=100: pos=0==kc => old_rightmost=INVALID, entry[0]=(INVALID,10), rightmost=100
    // Insert key=20, child=200: pos=1==kc => old_rightmost=100, entry[1]=(100,20), rightmost=200
    // Insert key=30, child=300: pos=2==kc => old_rightmost=200, entry[2]=(200,30), rightmost=300

    EXPECT_EQ(page.child_at(0), INVALID_PAGE_ID); // entry[0].child
    EXPECT_EQ(page.child_at(1), 100u); // entry[1].child
    EXPECT_EQ(page.child_at(2), 200u); // entry[2].child
    EXPECT_EQ(page.child_at(3), 300u); // rightmost

    // Search tests
    auto k5 = int32_key(5);
    auto k15 = int32_key(15);
    auto k25 = int32_key(25);
    auto k35 = int32_key(35);

    EXPECT_EQ(page.find_child(k5.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);
    EXPECT_EQ(page.find_child(k15.data(), sizeof(int32_t), int32_cmp), 100u);
    EXPECT_EQ(page.find_child(k25.data(), sizeof(int32_t), int32_cmp), 200u);
    EXPECT_EQ(page.find_child(k35.data(), sizeof(int32_t), int32_cmp), 300u);
}

TEST(BTreeInternalPageTest, InsertMultipleKeysUnsorted)
{
    BTreeInternalPage page;
    page.init(1, 0);

    // Set initial leftmost child for proper B+Tree semantics
    page.set_rightmost_child(999);

    // Insert keys in mixed order: 30, 10, 20 with children key*100
    for (int32_t v : {30, 10, 20})
    {
        auto k = int32_key(v);
        bool ok = page.insert(k.data(), sizeof(int32_t), v * 100, int32_cmp);
        EXPECT_TRUE(ok);
    }
    EXPECT_EQ(page.key_count(), 3u);

    // Verify keys are in order
    EXPECT_EQ(*reinterpret_cast<const int32_t *>(page.key_data_at(0)), 10);
    EXPECT_EQ(*reinterpret_cast<const int32_t *>(page.key_data_at(1)), 20);
    EXPECT_EQ(*reinterpret_cast<const int32_t *>(page.key_data_at(2)), 30);

    auto k5 = int32_key(5);
    auto k15 = int32_key(15);
    auto k25 = int32_key(25);
    auto k35 = int32_key(35);

    // All searches should return valid pages
    EXPECT_NE(page.find_child(k5.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);
    EXPECT_NE(page.find_child(k15.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);
    EXPECT_NE(page.find_child(k25.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);
    EXPECT_NE(page.find_child(k35.data(), sizeof(int32_t), int32_cmp), INVALID_PAGE_ID);
}

TEST(BTreeInternalPageTest, SplitHalfKeys)
{
    // Fill a page with many keys, then split
    BTreeInternalPage page;
    page.init(1, 0);

    // Insert keys 1..N, each with child = key*10
    constexpr int N = 20;
    for (int i = 1; i <= N; i++)
    {
        auto k = int32_key(i);
        bool ok = page.insert(k.data(), sizeof(int32_t), i * 10, int32_cmp);
        ASSERT_TRUE(ok) << "Failed to insert key " << i;
    }

    BTreeInternalPage new_page;
    auto [promoted_key, _] = page.split(&new_page);
    new_page.set_page_id(999);

    // Verify promoted key is the middle key (entry at N/2, 0-indexed)
    // With keys 1..20 and mid=10, promoted key is entry[10] = key 11
    int32_t promoted_val = *reinterpret_cast<const int32_t *>(promoted_key.data());
    EXPECT_EQ(promoted_val, N / 2 + 1); // mid=10, key at index 10 is 11

    // Verify old page has keys 1..mid (entries 0..mid-1)
    EXPECT_EQ(page.key_count(), static_cast<uint16_t>(N / 2));
    for (uint16_t i = 0; i < page.key_count(); i++)
    {
        int32_t key_val = *reinterpret_cast<const int32_t *>(page.key_data_at(i));
        EXPECT_EQ(key_val, static_cast<int32_t>(i + 1)) << "key at " << i;
    }

    // Verify new page has keys mid+2..N (entries mid+1..N-1)
    EXPECT_EQ(new_page.key_count(), static_cast<uint16_t>(N - N / 2 - 1));
    for (uint16_t i = 0; i < new_page.key_count(); i++)
    {
        int32_t key_val = *reinterpret_cast<const int32_t *>(new_page.key_data_at(i));
        EXPECT_EQ(key_val, static_cast<int32_t>(N / 2 + 2 + i)) << "key at " << i;
    }

    // Old page rightmost = mid_entry.child = (N/2) * 10
    EXPECT_EQ(page.rightmost_child(), static_cast<page_id_t>((N / 2) * 10));

    // New page rightmost = old rightmost = N * 10
    EXPECT_EQ(new_page.rightmost_child(), static_cast<page_id_t>(N * 10));
}

TEST(BTreeInternalPageTest, PageFull)
{
    BTreeInternalPage page;
    page.init(1, 0);

    // Insert until full
    int count = 0;
    for (int i = 1;; i++)
    {
        auto k = int32_key(i);
        bool ok = page.insert(k.data(), sizeof(int32_t), i * 10, int32_cmp);
        if (!ok)
            break;
        count++;
    }

    // PAGE_SIZE = 16384, each entry needs 12 bytes (8 entry + 4 key)
    // Should fit ~1360 entries
    EXPECT_GT(count, 1000);
    EXPECT_LT(count, 1400);

    // Verify we can still search
    auto k = int32_key(count / 2);
    page_id_t child = page.find_child(k.data(), sizeof(int32_t), int32_cmp);
    EXPECT_NE(child, INVALID_PAGE_ID);
}

// ============================================================================
// DataPage B+Tree Leaf Tests
// ============================================================================

TEST(DataPageBTreeTest, InitAsLeaf)
{
    DataPage page;
    page.init_as_leaf(10);

    EXPECT_EQ(page.header()->page_id, 10u);
    EXPECT_EQ(static_cast<PageType>(page.header()->page_type), PageType::BPTREE_LEAF_PAGE);
    EXPECT_EQ(page.slot_count(), 0u);
    EXPECT_EQ(page.next_leaf_page(), INVALID_PAGE_ID);
}

TEST(DataPageBTreeTest, InsertTupleSorted)
{
    DataPage page;
    page.init_as_leaf(1);

    // Comparator: compare first 4 bytes as int32
    auto cmp = [](const char * a, uint16_t al, const char * b, uint16_t bl) -> int
    {
        (void)al;
        (void)bl;
        int32_t va = *reinterpret_cast<const int32_t *>(a);
        int32_t vb = *reinterpret_cast<const int32_t *>(b);
        if (va < vb)
            return -1;
        if (va > vb)
            return 1;
        return 0;
    };

    // Insert in random order
    for (int32_t v : {30, 10, 50, 20, 40})
    {
        char buf[4];
        std::memcpy(buf, &v, 4);
        auto slot = page.insert_tuple_sorted(buf, 4, cmp);
        ASSERT_TRUE(slot.has_value());
    }

    // Verify sorted order
    int32_t expected[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++)
    {
        auto [data, len] = page.get_tuple(i);
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(len, 4u);
        EXPECT_EQ(*reinterpret_cast<const int32_t *>(data), expected[i]) << "slot " << i;
    }
}

TEST(DataPageBTreeTest, SplitLeafEntries)
{
    DataPage page;
    page.init_as_leaf(1);

    auto cmp = [](const char * a, uint16_t al, const char * b, uint16_t bl) -> int
    {
        (void)al;
        (void)bl;
        int32_t va = *reinterpret_cast<const int32_t *>(a);
        int32_t vb = *reinterpret_cast<const int32_t *>(b);
        if (va < vb)
            return -1;
        if (va > vb)
            return 1;
        return 0;
    };

    // Insert 10 entries
    for (int32_t v = 1; v <= 10; v++)
    {
        char buf[4];
        std::memcpy(buf, &v, 4);
        page.insert_tuple_sorted(buf, 4, cmp);
    }

    DataPage new_page;
    new_page.init_as_leaf(2);

    uint16_t moved = page.split_leaf_entries(&new_page);
    EXPECT_EQ(moved, 5u);

    // Old page should have first 5 entries (1-5)
    EXPECT_EQ(page.slot_count(), 10u); // slot_count unchanged, but half are DELETED

    // Compact old page
    page.compact();
    EXPECT_EQ(page.slot_count(), 5u);

    // New page should have 5 entries (6-10)
    EXPECT_EQ(new_page.slot_count(), 5u);

    // Verify old page data
    for (uint16_t i = 0; i < 5; i++)
    {
        auto [data, len] = page.get_tuple(i);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(*reinterpret_cast<const int32_t *>(data), i + 1);
    }

    // Verify new page data (6-10)
    for (uint16_t i = 0; i < 5; i++)
    {
        auto [data, len] = new_page.get_tuple(i);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(*reinterpret_cast<const int32_t *>(data), i + 6);
    }
}

TEST(DataPageBTreeTest, NextLeafPage)
{
    DataPage page;
    page.init_as_leaf(1);
    page.set_next_leaf_page(42);
    EXPECT_EQ(page.next_leaf_page(), 42u);
}

TEST(DataPageBTreeTest, CompactRemovesDeletedSlots)
{
    DataPage page;
    page.init_as_leaf(1);

    // Insert 5 entries
    for (int32_t v = 1; v <= 5; v++)
    {
        char buf[4];
        std::memcpy(buf, &v, 4);
        page.insert_tuple(buf, 4);
    }

    EXPECT_EQ(page.slot_count(), 5u);

    // Delete entries 1 and 3
    page.delete_tuple(1);
    page.delete_tuple(3);

    // Compact
    uint16_t removed = page.compact();
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(page.slot_count(), 3u);

    // Verify remaining entries: slots now hold values 1, 3, 5
    int32_t expected[] = {1, 3, 5};
    for (int i = 0; i < 3; i++)
    {
        auto [data, len] = page.get_tuple(i);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(*reinterpret_cast<const int32_t *>(data), expected[i]);
    }
}

TEST(DataPageBTreeTest, CompactNoOpWhenNoDeleted)
{
    DataPage page;
    page.init_as_leaf(1);

    for (int32_t v = 1; v <= 5; v++)
    {
        char buf[4];
        std::memcpy(buf, &v, 4);
        page.insert_tuple(buf, 4);
    }

    uint16_t removed = page.compact();
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(page.slot_count(), 5u);
}

TEST(DataPageBTreeTest, CompactAllDeletedYieldsZero)
{
    DataPage page;
    page.init_as_leaf(1);

    for (int32_t v = 1; v <= 5; v++)
    {
        char buf[4];
        std::memcpy(buf, &v, 4);
        page.insert_tuple(buf, 4);
    }

    for (uint16_t i = 0; i < 5; i++)
    {
        page.delete_tuple(i);
    }

    uint16_t removed = page.compact();
    EXPECT_EQ(removed, 5u);
    EXPECT_EQ(page.slot_count(), 0u);
}
