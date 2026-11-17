#include "sql_runner.h"
#include "bench/tpch/workload.h"

#include <cmath>
#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace
{

// Shared read-only TPC-H database populated once per test suite so all 22 TPC-H
// queries run individually and fast, plus dedicated mutation/recovery tests.
class TpchQueryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        std::random_device rd;
        suite_dir_ = std::filesystem::temp_directory_path() / ("popo_tpch_suite_" + std::to_string(rd()));
        std::filesystem::create_directories(suite_dir_);
        suite_runner_ = std::make_unique<popo::test::SqlRunner>(suite_dir_.string());
        create_tpch_schema(*suite_runner_);
        seed_tpch_data(*suite_runner_);
    }

    static void TearDownTestSuite()
    {
        suite_runner_.reset();
        std::filesystem::remove_all(suite_dir_);
    }

    static popo::test::SqlSuccess exec_checked(popo::test::SqlRunner & db, const std::string & sql)
    {
        auto res = db.exec(sql);
        EXPECT_TRUE(res) << sql << "\n  error: " << (res ? "" : res.error().error_message);
        return res ? std::move(*res) : popo::test::SqlSuccess{};
    }

    popo::test::SqlSuccess must(const std::string & sql)
    {
        return exec_checked(*suite_runner_, sql);
    }

    // Shared official TPC-H table definitions and primary-key policy.
    static void create_tpch_schema(popo::test::SqlRunner & db)
    {
        for (const auto & statement : popo::tpch::schema_statements(popo::tpch::Dialect::PopoDB))
            exec_checked(db, statement);
    }

    static void seed_tpch_data(popo::test::SqlRunner & db)
    {
        auto txn = db.begin();
        ASSERT_TRUE(txn) << (txn ? "" : txn.error().error_message);
        auto add = [&](const std::string & sql)
        {
            auto r = txn->exec(sql);
            ASSERT_TRUE(r) << sql << " -> " << (r ? "" : r.error().error_message);
        };

        add(R"(
            INSERT INTO region VALUES
                (0, 'AFRICA', 'r0'),
                (1, 'AMERICA', 'r1'),
                (2, 'ASIA', 'r2'),
                (3, 'EUROPE', 'r3'),
                (4, 'MIDDLE EAST', 'r4')
        )");

        add(R"(
            INSERT INTO nation VALUES
                (1,  'BRAZIL',        1, 'n1'),
                (3,  'CANADA',        1, 'n3'),
                (6,  'FRANCE',        3, 'n6'),
                (7,  'GERMANY',       3, 'n7'),
                (8,  'INDIA',         2, 'n8'),
                (12, 'JAPAN',         2, 'n12'),
                (20, 'SAUDI ARABIA',  4, 'n20'),
                (24, 'UNITED STATES', 1, 'n24')
        )");

        add(R"(
            INSERT INTO part VALUES
                (1, 'forest green antique',   'Manufacturer#1', 'Brand#23', 'ECONOMY ANODIZED STEEL', 15, 'MED BOX', 901.00, 'p1'),
                (2, 'blush brass bolt',       'Manufacturer#2', 'Brand#12', 'PROMO BURNISHED BRASS',   3, 'SM CASE', 902.00, 'p2'),
                (3, 'shiny green brass ring', 'Manufacturer#3', 'Brand#34', 'LARGE BRUSHED BRASS',    15, 'LG CASE', 903.00, 'p3')
        )");

        add(R"(
            INSERT INTO supplier VALUES
                (1, 'Supplier#000000001', 'Addr1',  7, '17-111-111-1111', 5000.00, 'reliable supplier'),
                (2, 'Supplier#000000002', 'Addr2',  6, '16-222-222-2222', 4000.00, 'good supplier'),
                (3, 'Supplier#000000003', 'Addr3',  1, '11-333-333-3333', 3000.00, 'brazil supplier'),
                (4, 'Supplier#000000004', 'Addr4',  3, '13-444-444-4444', 3500.00, 'canada supplier'),
                (5, 'Supplier#000000005', 'Addr5',  8, '18-555-555-5555', 4200.00, 'india supplier'),
                (6, 'Supplier#000000006', 'Addr6', 20, '30-666-666-6666', 4800.00, 'saudi supplier'),
                (7, 'Supplier#000000007', 'Addr7',  7, '17-777-777-7777',  100.00, 'bad Customer Complaints here')
        )");

        add(R"(
            INSERT INTO partsupp VALUES
                (3, 1, 100, 10.00, 'ps3_1'),
                (3, 2, 100, 15.00, 'ps3_2'),
                (3, 3, 100,  8.00, 'ps3_3'),
                (1, 1, 200, 20.00, 'ps1_1'),
                (1, 3,  50, 25.00, 'ps1_3'),
                (1, 4, 100, 30.00, 'ps1_4'),
                (2, 1,  50, 12.00, 'ps2_1'),
                (2, 2,  60, 14.00, 'ps2_2'),
                (2, 7,  10,  5.00, 'ps2_7')
        )");

        add(R"(
            INSERT INTO customer VALUES
                (1, 'Customer#000000001', 'CAddr1',  1, '13-111-111-1111', 1000.00, 'BUILDING',   'c1'),
                (2, 'Customer#000000002', 'CAddr2',  6, '16-222-222-2222', 2000.00, 'AUTOMOBILE', 'c2'),
                (3, 'Customer#000000003', 'CAddr3',  8, '18-333-333-3333', 3000.00, 'BUILDING',   'c3'),
                (4, 'Customer#000000004', 'CAddr4',  7, '17-444-444-4444',  500.00, 'MACHINERY',  'c4'),
                (5, 'Customer#000000005', 'CAddr5',  3, '13-555-555-5555', 9000.00, 'HOUSEHOLD',  'c5'),
                (6, 'Customer#000000006', 'CAddr6', 20, '30-666-666-6666',  100.00, 'FURNITURE',  'c6')
        )");

        add(R"(
            INSERT INTO orders VALUES
                (1, 1, 'O', 15000.00, '1995-03-01', '1-URGENT',        'Clerk#1', 0, 'regular order'),
                (2, 2, 'F',  8000.00, '1993-07-15', '2-HIGH',          'Clerk#2', 0, 'normal order'),
                (3, 1, 'F', 12000.00, '1993-10-15', '3-MEDIUM',        'Clerk#1', 0, 'returned items'),
                (4, 3, 'F', 20000.00, '1994-05-10', '1-URGENT',        'Clerk#3', 0, 'asia order'),
                (5, 4, 'O',  5000.00, '1995-09-05', '4-NOT SPECIFIED', 'Clerk#4', 0, 'special requests order'),
                (6, 4, 'O', 60000.00, '1996-01-15', '2-HIGH',          'Clerk#5', 0, 'bulk order'),
                (7, 2, 'F',  3000.00, '1995-05-01', '3-MEDIUM',        'Clerk#6', 0, 'waiting order')
        )");

        add(R"(
            INSERT INTO lineitem VALUES
                (1, 1, 3, 1,  10.00,  1000.00, 0.10, 0.05, 'N', 'O', '1995-03-20', '1995-03-25', '1995-03-30', 'NONE',              'TRUCK',   'l1_1'),
                (1, 1, 1, 2,  10.00,  2000.00, 0.00, 0.05, 'N', 'O', '1995-03-22', '1995-03-25', '1995-03-30', 'NONE',              'TRUCK',   'l1_2'),
                (2, 1, 1, 1,   2.00,   500.00, 0.05, 0.02, 'A', 'F', '1995-06-10', '1995-06-05', '1995-06-15', 'NONE',              'REG AIR', 'l2_1'),
                (3, 2, 4, 1,   8.00,  3000.00, 0.06, 0.04, 'R', 'F', '1994-02-10', '1994-02-20', '1994-02-25', 'DELIVER IN PERSON', 'MAIL',    'l3_1'),
                (4, 1, 5, 1,  80.00,  4000.00, 0.05, 0.03, 'A', 'F', '1994-05-20', '1994-05-25', '1994-05-28', 'NONE',              'SHIP',    'l4_1'),
                (4, 1, 4, 2,  80.00,  1600.00, 0.00, 0.02, 'A', 'F', '1994-06-01', '1994-06-10', '1994-06-05', 'NONE',              'TRUCK',   'l4_2'),
                (5, 2, 1, 1,   5.00,  1000.00, 0.10, 0.04, 'N', 'O', '1995-09-10', '1995-09-20', '1995-09-25', 'DELIVER IN PERSON', 'AIR',     'l5_1'),
                (5, 3, 2, 2,  10.00,  1000.00, 0.10, 0.04, 'N', 'O', '1995-09-12', '1995-09-20', '1995-09-25', 'NONE',              'TRUCK',   'l5_2'),
                (6, 3, 1, 1, 350.00, 35000.00, 0.00, 0.05, 'N', 'O', '1996-02-01', '1996-02-10', '1996-02-15', 'NONE',              'TRUCK',   'l6_1'),
                (7, 2, 6, 1,  10.00,  1000.00, 0.00, 0.00, 'A', 'F', '1995-05-10', '1995-05-12', '1995-05-20', 'NONE',              'TRUCK',   'l7_1'),
                (7, 2, 2, 2,  10.00,  1000.00, 0.00, 0.00, 'A', 'F', '1995-05-08', '1995-05-12', '1995-05-10', 'NONE',              'TRUCK',   'l7_2')
        )");

        auto committed = txn->commit();
        ASSERT_TRUE(committed) << (committed ? "" : committed.error().error_message);
    }

    static std::filesystem::path suite_dir_;
    static std::unique_ptr<popo::test::SqlRunner> suite_runner_;
};

std::filesystem::path TpchQueryTest::suite_dir_;
std::unique_ptr<popo::test::SqlRunner> TpchQueryTest::suite_runner_;

TEST_F(TpchQueryTest, Q01_PricingSummaryReport)
{
    auto query = popo::tpch::test_query(1);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "A");
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "F");
    EXPECT_EQ(r.result.rows[0]->values_[9].i64(), 5);
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "N");
    EXPECT_EQ(r.result.rows[1]->values_[1].string_ref(), "O");
    EXPECT_EQ(r.result.rows[1]->values_[9].i64(), 5);
    EXPECT_EQ(r.result.rows[2]->values_[0].string_ref(), "R");
    EXPECT_EQ(r.result.rows[2]->values_[1].string_ref(), "F");
    EXPECT_EQ(r.result.rows[2]->values_[9].i64(), 1);
}

TEST_F(TpchQueryTest, Q02_MinimumCostSupplier)
{
    auto query = popo::tpch::test_query(2);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "Supplier#000000001");
    EXPECT_EQ(r.result.rows[0]->values_[2].string_ref(), "GERMANY");
    EXPECT_EQ(r.result.rows[0]->values_[3].i32(), 3);
}

TEST_F(TpchQueryTest, Q03_ShippingPriority)
{
    auto query = popo::tpch::test_query(3);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 2900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q04_OrderPriorityChecking)
{
    auto query = popo::tpch::test_query(4);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "2-HIGH");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1);
}

TEST_F(TpchQueryTest, Q05_LocalSupplierVolume)
{
    auto query = popo::tpch::test_query(5);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "INDIA");
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 3800.0, 1e-6);
}

TEST_F(TpchQueryTest, Q06_ForecastingRevenueChange)
{
    auto query = popo::tpch::test_query(6);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 180.0, 1e-6);
}

TEST_F(TpchQueryTest, Q07_VolumeShipping)
{
    auto query = popo::tpch::test_query(7);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "FRANCE");
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "GERMANY");
    EXPECT_EQ(r.result.rows[0]->values_[2].i64(), 1995);
    EXPECT_NEAR(r.result.rows[0]->values_[3].to_f64(), 900.0, 1e-6);
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "GERMANY");
    EXPECT_EQ(r.result.rows[1]->values_[1].string_ref(), "FRANCE");
    EXPECT_NEAR(r.result.rows[1]->values_[3].to_f64(), 475.0, 1e-6);
}

TEST_F(TpchQueryTest, Q08_NationalMarketShare)
{
    auto query = popo::tpch::test_query(8);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i64(), 1995);
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 900.0 / 2900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q09_ProductTypeProfitMeasure)
{
    auto query = popo::tpch::test_query(9);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_FALSE(r.result.rows.empty());
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "BRAZIL");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1995);
    EXPECT_NEAR(r.result.rows[0]->values_[2].to_f64(), 650.0, 1e-6);
}

TEST_F(TpchQueryTest, Q10_ReturnedItemReporting)
{
    auto query = popo::tpch::test_query(10);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[2].to_f64(), 2820.0, 1e-6);
}

TEST_F(TpchQueryTest, Q11_ImportantStockIdentification)
{
    auto query = popo::tpch::test_query(11);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 4000.0, 1e-6);
    EXPECT_EQ(r.result.rows[1]->values_[0].i32(), 3);
    EXPECT_NEAR(r.result.rows[1]->values_[1].to_f64(), 1000.0, 1e-6);
    EXPECT_EQ(r.result.rows[2]->values_[0].i32(), 2);
    EXPECT_NEAR(r.result.rows[2]->values_[1].to_f64(), 650.0, 1e-6);
}

TEST_F(TpchQueryTest, Q12_ShippingModesAndOrderPriority)
{
    auto query = popo::tpch::test_query(12);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 2u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "MAIL");
    EXPECT_EQ(r.result.rows[0]->values_[1].to_f64(), 0.0);
    EXPECT_EQ(r.result.rows[0]->values_[2].to_f64(), 1.0);
    EXPECT_EQ(r.result.rows[1]->values_[0].string_ref(), "SHIP");
    EXPECT_EQ(r.result.rows[1]->values_[1].to_f64(), 1.0);
    EXPECT_EQ(r.result.rows[1]->values_[2].to_f64(), 0.0);
}

TEST_F(TpchQueryTest, Q13_CustomerDistribution)
{
    auto query = popo::tpch::test_query(13);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 3u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i64(), 2);
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 2);
    EXPECT_EQ(r.result.rows[1]->values_[0].i64(), 1);
    EXPECT_EQ(r.result.rows[1]->values_[1].i64(), 2);
    EXPECT_EQ(r.result.rows[2]->values_[0].i64(), 0);
    EXPECT_EQ(r.result.rows[2]->values_[1].i64(), 2);
}

TEST_F(TpchQueryTest, Q14_PromotionEffect)
{
    auto query = popo::tpch::test_query(14);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 50.0, 1e-6);
}

TEST_F(TpchQueryTest, Q15_TopSupplier)
{
    for (const auto & statement : popo::tpch::test_query_setup(15))
        must(statement);
    auto query = popo::tpch::test_query(15);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[4].to_f64(), 35000.0, 1e-6);
    for (const auto & statement : popo::tpch::test_query_cleanup(15))
        must(statement);
}

TEST_F(TpchQueryTest, Q16_PartsSupplierRelationship)
{
    auto query = popo::tpch::test_query(16);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Brand#12");
    EXPECT_EQ(r.result.rows[0]->values_[3].i64(), 2);
}

TEST_F(TpchQueryTest, Q17_SmallQuantityOrderRevenue)
{
    auto query = popo::tpch::test_query(17);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 500.0 / 7.0, 1e-6);
}

TEST_F(TpchQueryTest, Q18_LargeVolumeCustomer)
{
    auto query = popo::tpch::test_query(18);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[2].i32(), 6);
    EXPECT_NEAR(r.result.rows[0]->values_[5].to_f64(), 350.0, 1e-6);
}

TEST_F(TpchQueryTest, Q19_DiscountedRevenue)
{
    auto query = popo::tpch::test_query(19);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q20_PotentialPartPromotion)
{
    auto query = popo::tpch::test_query(20);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Supplier#000000004");
}

TEST_F(TpchQueryTest, Q21_SuppliersWhoKeptOrdersWaiting)
{
    auto query = popo::tpch::test_query(21);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Supplier#000000006");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1);
}

TEST_F(TpchQueryTest, Q22_GlobalSalesOpportunity)
{
    auto query = popo::tpch::test_query(22);
    ASSERT_TRUE(query) << (query ? "" : query.error().error_message);
    auto r = must(query ? *query : std::string{});
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "13");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[2].to_f64(), 9000.0, 1e-6);
}

TEST_F(TpchQueryTest, DialectVariants_PostgresDuckdbAndMysqlIntervals)
{
    // PostgreSQL / DuckDB single-string interval + MySQL DATE_ADD / DATE_SUB
    auto r = must(R"(
        SELECT
            DATE '1998-12-01' - INTERVAL '90 days',
            DATE '1993-07-01' + INTERVAL '3 months',
            DATE_SUB('1998-12-01', INTERVAL 90 DAY),
            DATE_ADD('1994-01-01', INTERVAL 1 YEAR),
            TRIM(BOTH 'x' FROM 'xxhelloxx')
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "1998-09-02");
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "1993-10-01");
    EXPECT_EQ(r.result.rows[0]->values_[2].string_ref(), "1998-09-02");
    EXPECT_EQ(r.result.rows[0]->values_[3].string_ref(), "1995-01-01");
    EXPECT_EQ(r.result.rows[0]->values_[4].string_ref(), "hello");
}

TEST_F(TpchQueryTest, RefreshFunctions_RF1_RF2_And_CompositePrimaryKey)
{
    must("INSERT INTO orders VALUES (99, 1, 'O', 500.00, '1997-01-01', '5-LOW', 'Clerk#9', 0, 'rf1')");
    must("INSERT INTO lineitem VALUES "
         "(99, 1, 1, 1, 3.00, 300.00, 0.00, 0.02, 'N', 'O', '1997-01-05', '1997-01-10', '1997-01-12', 'NONE', 'AIR', 'rf1_1'), "
         "(99, 2, 2, 2, 2.00, 200.00, 0.00, 0.02, 'N', 'O', '1997-01-06', '1997-01-10', '1997-01-12', 'NONE', 'AIR', 'rf1_2')");

    auto dup = suite_runner_->exec(
        "INSERT INTO lineitem VALUES "
        "(99, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', '1997-01-05', '1997-01-10', '1997-01-12', 'NONE', 'AIR', 'dup')");
    EXPECT_FALSE(dup);

    auto rf2_li = must("DELETE FROM lineitem WHERE l_orderkey = 99");
    EXPECT_EQ(rf2_li.result.rows_affected, 2);
    auto rf2_ord = must("DELETE FROM orders WHERE o_orderkey = 99");
    EXPECT_EQ(rf2_ord.result.rows_affected, 1);
}

TEST(TpchCompositeRecovery, SurvivesCrashAndReplay)
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("popo_tpch_rec_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    {
        auto * leaked = new popo::test::SqlRunner(dir.string(), 64);
        auto c = leaked->exec(
            "CREATE TABLE lineitem ("
            "l_orderkey INT NOT NULL, "
            "l_linenumber INT NOT NULL, "
            "l_extendedprice DECIMAL(15,2) NOT NULL, "
            "PRIMARY KEY (l_orderkey, l_linenumber))");
        ASSERT_TRUE(c) << (c ? "" : c.error().error_message);
        auto ins = leaked->exec("INSERT INTO lineitem VALUES (10, 1, 100.50), (10, 2, 200.25), (11, 1, 50.00)");
        ASSERT_TRUE(ins) << (ins ? "" : ins.error().error_message);
        auto del = leaked->exec("DELETE FROM lineitem WHERE l_orderkey = 10 AND l_linenumber = 1");
        ASSERT_TRUE(del) << (del ? "" : del.error().error_message);
        leaked->engine().buffer_pool()->invalidate_all();
    }

    popo::storage::StorageEngine recovered(dir.string(), 64);
    ASSERT_TRUE(recovered.init());
    auto rows = recovered.mvcc()->current_rows("lineitem");
    ASSERT_EQ(rows.size(), 2u);
    recovered.shutdown();
    std::filesystem::remove_all(dir);
}

TEST(TpchCatalogRecovery, TablesAndIndexesSurviveRestart)
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("popo_tpch_cat_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    {
        popo::test::SqlRunner db(dir.string(), 64);
        auto c = db.exec(
            "CREATE TABLE lineitem ("
            "l_orderkey INT NOT NULL, "
            "l_linenumber INT NOT NULL, "
            "l_suppkey INT NOT NULL, "
            "PRIMARY KEY (l_orderkey, l_linenumber))");
        ASSERT_TRUE(c) << (c ? "" : c.error().error_message);
        auto idx = db.exec("CREATE INDEX li_supp ON lineitem (l_suppkey)");
        ASSERT_TRUE(idx) << (idx ? "" : idx.error().error_message);
        auto ins = db.exec("INSERT INTO lineitem VALUES (1, 1, 7), (1, 2, 8), (2, 1, 7)");
        ASSERT_TRUE(ins) << (ins ? "" : ins.error().error_message);
    }

    popo::test::SqlRunner reopened(dir.string(), 64);
    auto tables = reopened.exec("SHOW TABLES");
    ASSERT_TRUE(tables) << (tables ? "" : tables.error().error_message);
    ASSERT_EQ(tables->result.rows.size(), 1u);
    EXPECT_EQ(tables->result.rows[0]->values_[0].string_ref(), "lineitem");

    auto rows = reopened.exec("SELECT l_linenumber FROM lineitem WHERE l_orderkey = 1 ORDER BY l_linenumber");
    ASSERT_TRUE(rows) << (rows ? "" : rows.error().error_message);
    ASSERT_EQ(rows->result.rows.size(), 2u);
    EXPECT_EQ(rows->result.rows[1]->values_[0].i32(), 2);

    auto indexed = reopened.exec("SELECT l_orderkey FROM lineitem WHERE l_suppkey = 7 ORDER BY l_orderkey");
    ASSERT_TRUE(indexed) << (indexed ? "" : indexed.error().error_message);
    EXPECT_EQ(indexed->result.rows.size(), 2u);

    auto listed = reopened.schema().list_indexes("lineitem");
    ASSERT_TRUE(listed) << (listed ? "" : listed.error().error_message);
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ((*listed)[0].index_name_, "li_supp");

    reopened.engine().shutdown();
    std::filesystem::remove_all(dir);
}

} // namespace
