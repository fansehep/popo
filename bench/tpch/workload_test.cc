#include "bench/tpch/workload.h"

#include <fstream>
#include <random>

#include <gtest/gtest.h>

namespace
{

TEST(TpchWorkload, HasOfficialTablesAndIdenticalPrimaryKeyPolicy)
{
    ASSERT_EQ(popo::tpch::tables().size(), 8u);
    auto popodb = popo::tpch::schema_statements(popo::tpch::Dialect::PopoDB);
    auto sqlite = popo::tpch::schema_statements(popo::tpch::Dialect::SQLite);
    EXPECT_EQ(popodb, sqlite);
    ASSERT_EQ(popodb.size(), 8u);
    for (const auto & statement : popodb)
        EXPECT_NE(statement.find("PRIMARY KEY"), std::string::npos);
}

TEST(TpchWorkload, ParsesTrailingDelimiterWithoutKeepingPhantomColumn)
{
    auto row = popo::tpch::read_tbl_row("1|O'Brien||", 3);
    ASSERT_TRUE(row) << (row ? "" : row.error().error_message);
    EXPECT_EQ(*row, (std::vector<std::string>{"1", "O'Brien", ""}));
    EXPECT_EQ(popo::tpch::insert_sql("sample", *row), "INSERT INTO sample VALUES ('1','O''Brien','')");

    auto malformed = popo::tpch::read_tbl_row("1|two", 2);
    EXPECT_FALSE(malformed);
}

TEST(TpchWorkload, ConvertsOfficialDateExtractAndSubstringSyntaxForSQLite)
{
    auto converted = popo::tpch::to_dialect(
        "select date '1998-12-01' - interval '90' day (3), "
        "extract(year from o_orderdate), substring(c_phone from 1 for 2)",
        popo::tpch::Dialect::SQLite);
    EXPECT_NE(converted.find("date('1998-12-01','-90 day')"), std::string::npos);
    EXPECT_NE(converted.find("CAST(strftime('%Y',o_orderdate) AS INTEGER)"), std::string::npos);
    EXPECT_NE(converted.find("substr(c_phone,1,2)"), std::string::npos);
}

TEST(TpchWorkload, SharesAllDeterministicQueries)
{
    for (size_t query = 1; query <= 22; ++query)
    {
        auto sql = popo::tpch::test_query(query);
        ASSERT_TRUE(sql) << query << ": " << (sql ? "" : sql.error().error_message);
        EXPECT_NE(sql->find("select"), std::string::npos);
    }
    EXPECT_FALSE(popo::tpch::test_query(0));
    EXPECT_FALSE(popo::tpch::test_query(23));
}

TEST(TpchWorkload, PassesScaleToQgenForEveryQuery)
{
    std::random_device random;
    auto root = std::filesystem::temp_directory_path() / ("popo_qgen_" + std::to_string(random()));
    ASSERT_TRUE(std::filesystem::create_directories(root));
    auto script = root / "qgen";
    {
        std::ofstream out(script);
        ASSERT_TRUE(out);
        out << "#!/bin/sh\n"
               "[ \"$1\" = \"-d\" ] || exit 3\n"
               "[ \"$2\" = \"-N\" ] || exit 4\n"
               "[ \"$3\" = \"-s\" ] || exit 5\n"
               "[ \"$4\" = \"10\" ] || exit 6\n"
               "[ -n \"$DSS_QUERY\" ] || exit 7\n"
               "printf 'select %s;\\n' \"$5\"\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    popo::tpch::GeneratorConfig config;
    config.qgen = script;
    config.scale_factor = 10;
    auto queries = popo::tpch::qgen_queries(config, popo::tpch::Dialect::PopoDB);
    ASSERT_TRUE(queries) << (queries ? "" : queries.error().error_message);
    EXPECT_EQ((*queries)[0], "select 1;\n");
    EXPECT_EQ((*queries)[10], "select 11;\n");
    EXPECT_EQ((*queries)[21], "select 22;\n");
    std::filesystem::remove_all(root);
}

} // namespace
