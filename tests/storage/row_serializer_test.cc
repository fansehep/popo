#include <gtest/gtest.h>
#include "src/catalog/column.h"
#include "src/catalog/schema.h"
#include "src/storage/row_serializer.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

using namespace popo::storage;
using namespace popo::type;
using namespace popo::catalog;

namespace
{

Schema make_schema(const std::vector<std::pair<std::string, ValueType>> & cols)
{
    Schema schema;
    schema.id_ = 1;
    schema.table_name_ = "test_table";
    schema.primary_key_col_idx_ = 0;
    for (auto & [name, type] : cols)
    {
        Column col;
        col.column_name_ = name;
        col.data_type_ = type;
        col.is_primary_key_ = false;
        col.is_nullable_ = true;
        col.is_unique_ = false;
        col.type_len_ = (type == ValueType::String) ? 256 : 4;
        schema.columns_.push_back(col);
    }
    return schema;
}

} // anonymous namespace

class RowSerializerTest : public ::testing::Test
{
};

TEST_F(RowSerializerTest, SerializeDeserializeIntegers)
{
    auto schema = make_schema({{"id", ValueType::Int32}, {"big_id", ValueType::Int64}});
    Row row;
    row.append_value(Value(ValueType::Int32, int32_t(42)));
    row.append_value(Value(ValueType::Int64, int64_t(123456789LL)));

    auto bytes = RowSerializer::serialize(row, schema);
    ASSERT_FALSE(bytes.empty());

    auto restored = RowSerializer::deserialize(bytes.data(), static_cast<uint32_t>(bytes.size()), schema);
    ASSERT_EQ(restored.len(), 2u);
    EXPECT_EQ(restored.values_[0].i32(), 42);
    EXPECT_EQ(restored.values_[1].i64(), 123456789LL);
}

TEST_F(RowSerializerTest, SerializeDeserializeStrings)
{
    auto schema = make_schema({{"name", ValueType::String}, {"city", ValueType::String}});
    Row row;
    row.append_value(Value(ValueType::String, std::string("Alice")));
    row.append_value(Value(ValueType::String, std::string("Beijing")));

    auto bytes = RowSerializer::serialize(row, schema);
    auto restored = RowSerializer::deserialize(bytes.data(), static_cast<uint32_t>(bytes.size()), schema);
    ASSERT_EQ(restored.len(), 2u);
    EXPECT_EQ(restored.values_[0].string_ref(), "Alice");
    EXPECT_EQ(restored.values_[1].string_ref(), "Beijing");
}

TEST_F(RowSerializerTest, SerializeDeserializeMixed)
{
    auto schema
        = make_schema({{"id", ValueType::Int32}, {"name", ValueType::String}, {"score", ValueType::Float32}, {"active", ValueType::Bool}});
    Row row;
    row.append_value(Value(ValueType::Int32, int32_t(7)));
    row.append_value(Value(ValueType::String, std::string("Bob")));
    row.append_value(Value(ValueType::Float32, 3.14f));
    row.append_value(Value(ValueType::Bool, true));

    auto bytes = RowSerializer::serialize(row, schema);
    auto restored = RowSerializer::deserialize(bytes.data(), static_cast<uint32_t>(bytes.size()), schema);
    ASSERT_EQ(restored.len(), 4u);
    EXPECT_EQ(restored.values_[0].i32(), 7);
    EXPECT_EQ(restored.values_[1].string_ref(), "Bob");
    EXPECT_FLOAT_EQ(restored.values_[2].f32(), 3.14f);
    EXPECT_EQ(restored.values_[3].boolean(), true);
}

TEST_F(RowSerializerTest, SerializeDeserializeWithNulls)
{
    auto schema = make_schema({{"id", ValueType::Int32}, {"name", ValueType::String}, {"age", ValueType::Int32}});
    Row row;
    row.append_value(Value(ValueType::Int32, int32_t(1)));
    row.append_value(Value()); // Null
    row.append_value(Value()); // Null

    auto bytes = RowSerializer::serialize(row, schema);
    auto restored = RowSerializer::deserialize(bytes.data(), static_cast<uint32_t>(bytes.size()), schema);
    ASSERT_EQ(restored.len(), 3u);
    EXPECT_EQ(restored.values_[0].i32(), 1);
    EXPECT_EQ(restored.values_[1].datatype(), ValueType::Null);
    EXPECT_EQ(restored.values_[2].datatype(), ValueType::Null);
}

TEST_F(RowSerializerTest, SerializedSizeConsistency)
{
    auto schema = make_schema({{"id", ValueType::Int32}, {"name", ValueType::String}});
    Row row;
    row.append_value(Value(ValueType::Int32, int32_t(99)));
    row.append_value(Value(ValueType::String, std::string("consistency")));

    auto predicted = RowSerializer::serialized_size(row, schema);
    auto bytes = RowSerializer::serialize(row, schema);
    EXPECT_EQ(predicted, static_cast<uint32_t>(bytes.size()));
}

TEST_F(RowSerializerTest, EmptyRow)
{
    auto schema = make_schema({});
    Row row;

    auto bytes = RowSerializer::serialize(row, schema);
    auto restored = RowSerializer::deserialize(bytes.data(), static_cast<uint32_t>(bytes.size()), schema);
    EXPECT_EQ(restored.len(), 0u);
}
