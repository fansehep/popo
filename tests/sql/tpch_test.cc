#include "sql_runner.h"

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

    // Official TPC-H dss.ddl + dss.ri (composite PKs, DECIMAL, DATE, CHAR, CHARACTER VARYING, FOREIGN KEYs)
    static void create_tpch_schema(popo::test::SqlRunner & db)
    {
        exec_checked(db, R"(
            CREATE TABLE REGION (
                R_REGIONKEY INTEGER NOT NULL,
                R_NAME      CHAR(25) NOT NULL,
                R_COMMENT   VARCHAR(152)
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE NATION (
                N_NATIONKEY INTEGER NOT NULL,
                N_NAME      CHARACTER(25) NOT NULL,
                N_REGIONKEY INTEGER NOT NULL,
                N_COMMENT   CHARACTER VARYING(152)
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE PART (
                P_PARTKEY     INTEGER NOT NULL,
                P_NAME        VARCHAR(55) NOT NULL,
                P_MFGR        CHAR(25) NOT NULL,
                P_BRAND       CHAR(10) NOT NULL,
                P_TYPE        VARCHAR(25) NOT NULL,
                P_SIZE        INTEGER NOT NULL,
                P_CONTAINER   CHAR(10) NOT NULL,
                P_RETAILPRICE DECIMAL(15,2) NOT NULL,
                P_COMMENT     VARCHAR(23) NOT NULL
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE SUPPLIER (
                S_SUPPKEY   INTEGER NOT NULL,
                S_NAME      CHAR(25) NOT NULL,
                S_ADDRESS   VARCHAR(40) NOT NULL,
                S_NATIONKEY INTEGER NOT NULL,
                S_PHONE     CHAR(15) NOT NULL,
                S_ACCTBAL   DECIMAL(15,2) NOT NULL,
                S_COMMENT   VARCHAR(101) NOT NULL
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE PARTSUPP (
                PS_PARTKEY    INTEGER NOT NULL,
                PS_SUPPKEY    INTEGER NOT NULL,
                PS_AVAILQTY   INTEGER NOT NULL,
                PS_SUPPLYCOST DECIMAL(15,2) NOT NULL,
                PS_COMMENT    VARCHAR(199) NOT NULL
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE CUSTOMER (
                C_CUSTKEY    INTEGER NOT NULL,
                C_NAME       CHAR(25) NOT NULL,
                C_ADDRESS    VARCHAR(40) NOT NULL,
                C_NATIONKEY  INTEGER NOT NULL,
                C_PHONE      CHAR(15) NOT NULL,
                C_ACCTBAL    DECIMAL(15,2) NOT NULL,
                C_MKTSEGMENT CHAR(10) NOT NULL,
                C_COMMENT    VARCHAR(117) NOT NULL
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE ORDERS (
                O_ORDERKEY      INTEGER NOT NULL,
                O_CUSTKEY       INTEGER NOT NULL,
                O_ORDERSTATUS   CHAR(1) NOT NULL,
                O_TOTALPRICE    DECIMAL(15,2) NOT NULL,
                O_ORDERDATE     DATE NOT NULL,
                O_ORDERPRIORITY CHAR(15) NOT NULL,
                O_CLERK         CHAR(15) NOT NULL,
                O_SHIPPRIORITY  INTEGER NOT NULL,
                O_COMMENT       VARCHAR(79) NOT NULL
            )
        )");
        exec_checked(db, R"(
            CREATE TABLE LINEITEM (
                L_ORDERKEY      INTEGER NOT NULL,
                L_PARTKEY       INTEGER NOT NULL,
                L_SUPPKEY       INTEGER NOT NULL,
                L_LINENUMBER    INTEGER NOT NULL,
                L_QUANTITY      DECIMAL(15,2) NOT NULL,
                L_EXTENDEDPRICE DECIMAL(15,2) NOT NULL,
                L_DISCOUNT      DECIMAL(15,2) NOT NULL,
                L_TAX           DECIMAL(15,2) NOT NULL,
                L_RETURNFLAG    CHAR(1) NOT NULL,
                L_LINESTATUS    CHAR(1) NOT NULL,
                L_SHIPDATE      DATE NOT NULL,
                L_COMMITDATE    DATE NOT NULL,
                L_RECEIPTDATE   DATE NOT NULL,
                L_SHIPINSTRUCT  CHAR(25) NOT NULL,
                L_SHIPMODE      CHAR(10) NOT NULL,
                L_COMMENT       VARCHAR(44) NOT NULL
            )
        )");

        exec_checked(db, "ALTER TABLE REGION ADD PRIMARY KEY (R_REGIONKEY)");
        exec_checked(db, "ALTER TABLE NATION ADD PRIMARY KEY (N_NATIONKEY)");
        exec_checked(db, "ALTER TABLE PART ADD PRIMARY KEY (P_PARTKEY)");
        exec_checked(db, "ALTER TABLE SUPPLIER ADD PRIMARY KEY (S_SUPPKEY)");
        exec_checked(db, "ALTER TABLE PARTSUPP ADD PRIMARY KEY (PS_PARTKEY, PS_SUPPKEY)");
        exec_checked(db, "ALTER TABLE CUSTOMER ADD PRIMARY KEY (C_CUSTKEY)");
        exec_checked(db, "ALTER TABLE ORDERS ADD PRIMARY KEY (O_ORDERKEY)");
        exec_checked(db, "ALTER TABLE LINEITEM ADD PRIMARY KEY (L_ORDERKEY, L_LINENUMBER)");

        exec_checked(db, "ALTER TABLE NATION ADD FOREIGN KEY NATION_FK1 (N_REGIONKEY) REFERENCES REGION(R_REGIONKEY)");
        exec_checked(db, "ALTER TABLE SUPPLIER ADD FOREIGN KEY SUPPLIER_FK1 (S_NATIONKEY) REFERENCES NATION(N_NATIONKEY)");
        exec_checked(db, "ALTER TABLE CUSTOMER ADD FOREIGN KEY CUSTOMER_FK1 (C_NATIONKEY) REFERENCES NATION(N_NATIONKEY)");
        exec_checked(db, "ALTER TABLE PARTSUPP ADD FOREIGN KEY PARTSUPP_FK1 (PS_SUPPKEY) REFERENCES SUPPLIER(S_SUPPKEY)");
        exec_checked(db, "ALTER TABLE PARTSUPP ADD FOREIGN KEY PARTSUPP_FK2 (PS_PARTKEY) REFERENCES PART(P_PARTKEY)");
        exec_checked(db, "ALTER TABLE ORDERS ADD FOREIGN KEY ORDERS_FK1 (O_CUSTKEY) REFERENCES CUSTOMER(C_CUSTKEY)");
        exec_checked(db, "ALTER TABLE LINEITEM ADD FOREIGN KEY LINEITEM_FK1 (L_ORDERKEY) REFERENCES ORDERS(O_ORDERKEY)");
        exec_checked(db, "ALTER TABLE LINEITEM ADD FOREIGN KEY LINEITEM_FK2 (L_PARTKEY, L_SUPPKEY) REFERENCES PARTSUPP(PS_PARTKEY, PS_SUPPKEY)");
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
    auto r = must(R"(
        select
            l_returnflag,
            l_linestatus,
            sum(l_quantity) as sum_qty,
            sum(l_extendedprice) as sum_base_price,
            sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
            sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
            avg(l_quantity) as avg_qty,
            avg(l_extendedprice) as avg_price,
            avg(l_discount) as avg_disc,
            count(*) as count_order
        from
            lineitem
        where
            l_shipdate <= date '1998-12-01' - interval '90' day (3)
        group by
            l_returnflag,
            l_linestatus
        order by
            l_returnflag,
            l_linestatus
    )");
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
    auto r = must(R"(
        select
            s_acctbal,
            s_name,
            n_name,
            p_partkey,
            p_mfgr,
            s_address,
            s_phone,
            s_comment
        from
            part,
            supplier,
            partsupp,
            nation,
            region
        where
            p_partkey = ps_partkey
            and s_suppkey = ps_suppkey
            and p_size = 15
            and p_type like '%BRASS'
            and s_nationkey = n_nationkey
            and n_regionkey = r_regionkey
            and r_name = 'EUROPE'
            and ps_supplycost = (
                select
                    min(ps_supplycost)
                from
                    partsupp,
                    supplier,
                    nation,
                    region
                where
                    p_partkey = ps_partkey
                    and s_suppkey = ps_suppkey
                    and s_nationkey = n_nationkey
                    and n_regionkey = r_regionkey
                    and r_name = 'EUROPE'
            )
        order by
            s_acctbal desc,
            n_name,
            s_name,
            p_partkey
        limit 100
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[1].string_ref(), "Supplier#000000001");
    EXPECT_EQ(r.result.rows[0]->values_[2].string_ref(), "GERMANY");
    EXPECT_EQ(r.result.rows[0]->values_[3].i32(), 3);
}

TEST_F(TpchQueryTest, Q03_ShippingPriority)
{
    auto r = must(R"(
        select
            l_orderkey,
            sum(l_extendedprice * (1 - l_discount)) as revenue,
            o_orderdate,
            o_shippriority
        from
            customer,
            orders,
            lineitem
        where
            c_mktsegment = 'BUILDING'
            and c_custkey = o_custkey
            and l_orderkey = o_orderkey
            and o_orderdate < date '1995-03-15'
            and l_shipdate > date '1995-03-15'
        group by
            l_orderkey,
            o_orderdate,
            o_shippriority
        order by
            revenue desc,
            o_orderdate
        limit 10
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 2900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q04_OrderPriorityChecking)
{
    auto r = must(R"(
        select
            o_orderpriority,
            count(*) as order_count
        from
            orders
        where
            o_orderdate >= date '1993-07-01'
            and o_orderdate < date '1993-07-01' + interval '3' month
            and exists (
                select
                    *
                from
                    lineitem
                where
                    l_orderkey = o_orderkey
                    and l_commitdate < l_receiptdate
            )
        group by
            o_orderpriority
        order by
            o_orderpriority
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "2-HIGH");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1);
}

TEST_F(TpchQueryTest, Q05_LocalSupplierVolume)
{
    auto r = must(R"(
        select
            n_name,
            sum(l_extendedprice * (1 - l_discount)) as revenue
        from
            customer,
            orders,
            lineitem,
            supplier,
            nation,
            region
        where
            c_custkey = o_custkey
            and l_orderkey = o_orderkey
            and l_suppkey = s_suppkey
            and c_nationkey = s_nationkey
            and s_nationkey = n_nationkey
            and n_regionkey = r_regionkey
            and r_name = 'ASIA'
            and o_orderdate >= date '1994-01-01'
            and o_orderdate < date '1994-01-01' + interval '1' year
        group by
            n_name
        order by
            revenue desc
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "INDIA");
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 3800.0, 1e-6);
}

TEST_F(TpchQueryTest, Q06_ForecastingRevenueChange)
{
    auto r = must(R"(
        select
            sum(l_extendedprice * l_discount) as revenue
        from
            lineitem
        where
            l_shipdate >= date '1994-01-01'
            and l_shipdate < date '1994-01-01' + interval '1' year
            and l_discount between .06 - 0.01 and .06 + 0.01
            and l_quantity < 24
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 180.0, 1e-6);
}

TEST_F(TpchQueryTest, Q07_VolumeShipping)
{
    auto r = must(R"(
        select
            supp_nation,
            cust_nation,
            l_year,
            sum(volume) as revenue
        from (
            select
                n1.n_name as supp_nation,
                n2.n_name as cust_nation,
                extract(year from l_shipdate) as l_year,
                l_extendedprice * (1 - l_discount) as volume
            from
                supplier,
                lineitem,
                orders,
                customer,
                nation n1,
                nation n2
            where
                s_suppkey = l_suppkey
                and o_orderkey = l_orderkey
                and c_custkey = o_custkey
                and s_nationkey = n1.n_nationkey
                and c_nationkey = n2.n_nationkey
                and (
                    (n1.n_name = 'FRANCE' and n2.n_name = 'GERMANY')
                    or (n1.n_name = 'GERMANY' and n2.n_name = 'FRANCE')
                )
                and l_shipdate between date '1995-01-01' and date '1996-12-31'
        ) as shipping
        group by
            supp_nation,
            cust_nation,
            l_year
        order by
            supp_nation,
            cust_nation,
            l_year
    )");
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
    auto r = must(R"(
        select
            o_year,
            sum(case
                when nation = 'BRAZIL' then volume
                else 0
            end) / sum(volume) as mkt_share
        from (
            select
                extract(year from o_orderdate) as o_year,
                l_extendedprice * (1 - l_discount) as volume,
                n2.n_name as nation
            from
                part,
                supplier,
                lineitem,
                orders,
                customer,
                nation n1,
                nation n2,
                region
            where
                p_partkey = l_partkey
                and s_suppkey = l_suppkey
                and l_orderkey = o_orderkey
                and o_custkey = c_custkey
                and c_nationkey = n1.n_nationkey
                and n1.n_regionkey = r_regionkey
                and r_name = 'AMERICA'
                and s_nationkey = n2.n_nationkey
                and o_orderdate between date '1995-01-01' and date '1996-12-31'
                and p_type = 'ECONOMY ANODIZED STEEL'
        ) as all_nations
        group by
            o_year
        order by
            o_year
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i64(), 1995);
    EXPECT_NEAR(r.result.rows[0]->values_[1].to_f64(), 900.0 / 2900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q09_ProductTypeProfitMeasure)
{
    auto r = must(R"(
        select
            nation,
            o_year,
            sum(amount) as sum_profit
        from (
            select
                n_name as nation,
                extract(year from o_orderdate) as o_year,
                l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity as amount
            from
                part,
                supplier,
                lineitem,
                partsupp,
                orders,
                nation
            where
                s_suppkey = l_suppkey
                and ps_suppkey = l_suppkey
                and ps_partkey = l_partkey
                and p_partkey = l_partkey
                and o_orderkey = l_orderkey
                and s_nationkey = n_nationkey
                and p_name like '%green%'
        ) as profit
        group by
            nation,
            o_year
        order by
            nation,
            o_year desc
    )");
    ASSERT_FALSE(r.result.rows.empty());
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "BRAZIL");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1995);
    EXPECT_NEAR(r.result.rows[0]->values_[2].to_f64(), 650.0, 1e-6);
}

TEST_F(TpchQueryTest, Q10_ReturnedItemReporting)
{
    auto r = must(R"(
        select
            c_custkey,
            c_name,
            sum(l_extendedprice * (1 - l_discount)) as revenue,
            c_acctbal,
            n_name,
            c_address,
            c_phone,
            c_comment
        from
            customer,
            orders,
            lineitem,
            nation
        where
            c_custkey = o_custkey
            and l_orderkey = o_orderkey
            and o_orderdate >= date '1993-10-01'
            and o_orderdate < date '1993-10-01' + interval '3' month
            and l_returnflag = 'R'
            and c_nationkey = n_nationkey
        group by
            c_custkey,
            c_name,
            c_acctbal,
            c_phone,
            n_name,
            c_address,
            c_comment
        order by
            revenue desc
        limit 20
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[2].to_f64(), 2820.0, 1e-6);
}

TEST_F(TpchQueryTest, Q11_ImportantStockIdentification)
{
    auto r = must(R"(
        select
            ps_partkey,
            sum(ps_supplycost * ps_availqty) as value
        from
            partsupp,
            supplier,
            nation
        where
            ps_suppkey = s_suppkey
            and s_nationkey = n_nationkey
            and n_name = 'GERMANY'
        group by
            ps_partkey
        having
            sum(ps_supplycost * ps_availqty) > (
                select
                    sum(ps_supplycost * ps_availqty) * 0.0001000000
                from
                    partsupp,
                    supplier,
                    nation
                where
                    ps_suppkey = s_suppkey
                    and s_nationkey = n_nationkey
                    and n_name = 'GERMANY'
            )
        order by
            value desc
    )");
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
    auto r = must(R"(
        select
            l_shipmode,
            sum(case
                when o_orderpriority = '1-URGENT'
                    or o_orderpriority = '2-HIGH'
                    then 1
                else 0
            end) as high_line_count,
            sum(case
                when o_orderpriority <> '1-URGENT'
                    and o_orderpriority <> '2-HIGH'
                    then 1
                else 0
            end) as low_line_count
        from
            orders,
            lineitem
        where
            o_orderkey = l_orderkey
            and l_shipmode in ('MAIL', 'SHIP')
            and l_commitdate < l_receiptdate
            and l_shipdate < l_commitdate
            and l_receiptdate >= date '1994-01-01'
            and l_receiptdate < date '1994-01-01' + interval '1' year
        group by
            l_shipmode
        order by
            l_shipmode
    )");
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
    auto r = must(R"(
        select
            c_count,
            count(*) as custdist
        from (
            select
                c_custkey,
                count(o_orderkey)
            from
                customer left outer join orders on
                    c_custkey = o_custkey
                    and o_comment not like '%special%requests%'
            group by
                c_custkey
        ) as c_orders (c_custkey, c_count)
        group by
            c_count
        order by
            custdist desc,
            c_count desc
    )");
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
    auto r = must(R"(
        select
            100.00 * sum(case
                when p_type like 'PROMO%'
                    then l_extendedprice * (1 - l_discount)
                else 0
            end) / sum(l_extendedprice * (1 - l_discount)) as promo_revenue
        from
            lineitem,
            part
        where
            l_partkey = p_partkey
            and l_shipdate >= date '1995-09-01'
            and l_shipdate < date '1995-09-01' + interval '1' month
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 50.0, 1e-6);
}

TEST_F(TpchQueryTest, Q15_TopSupplier)
{
    must(R"(
        create view revenue0 (supplier_no, total_revenue) as
            select
                l_suppkey,
                sum(l_extendedprice * (1 - l_discount))
            from
                lineitem
            where
                l_shipdate >= date '1996-01-01'
                and l_shipdate < date '1996-01-01' + interval '3' month
            group by
                l_suppkey
    )");
    auto r = must(R"(
        select
            s_suppkey,
            s_name,
            s_address,
            s_phone,
            total_revenue
        from
            supplier,
            revenue0
        where
            s_suppkey = supplier_no
            and total_revenue = (
                select
                    max(total_revenue)
                from
                    revenue0
            )
        order by
            s_suppkey
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].i32(), 1);
    EXPECT_NEAR(r.result.rows[0]->values_[4].to_f64(), 35000.0, 1e-6);
    must("drop view revenue0");
}

TEST_F(TpchQueryTest, Q16_PartsSupplierRelationship)
{
    auto r = must(R"(
        select
            p_brand,
            p_type,
            p_size,
            count(distinct ps_suppkey) as supplier_cnt
        from
            partsupp,
            part
        where
            p_partkey = ps_partkey
            and p_brand <> 'Brand#45'
            and p_type not like 'MEDIUM POLISHED%'
            and p_size in (49, 14, 23, 45, 19, 3, 36, 9)
            and ps_suppkey not in (
                select
                    s_suppkey
                from
                    supplier
                where
                    s_comment like '%Customer%Complaints%'
            )
        group by
            p_brand,
            p_type,
            p_size
        order by
            supplier_cnt desc,
            p_brand,
            p_type,
            p_size
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Brand#12");
    EXPECT_EQ(r.result.rows[0]->values_[3].i64(), 2);
}

TEST_F(TpchQueryTest, Q17_SmallQuantityOrderRevenue)
{
    auto r = must(R"(
        select
            sum(l_extendedprice) / 7.0 as avg_yearly
        from
            lineitem,
            part
        where
            p_partkey = l_partkey
            and p_brand = 'Brand#23'
            and p_container = 'MED BOX'
            and l_quantity < (
                select
                    0.2 * avg(l_quantity)
                from
                    lineitem
                where
                    l_partkey = p_partkey
            )
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 500.0 / 7.0, 1e-6);
}

TEST_F(TpchQueryTest, Q18_LargeVolumeCustomer)
{
    auto r = must(R"(
        select
            c_name,
            c_custkey,
            o_orderkey,
            o_orderdate,
            o_totalprice,
            sum(l_quantity)
        from
            customer,
            orders,
            lineitem
        where
            o_orderkey in (
                select
                    l_orderkey
                from
                    lineitem
                group by
                    l_orderkey having
                        sum(l_quantity) > 300
            )
            and c_custkey = o_custkey
            and o_orderkey = l_orderkey
        group by
            c_name,
            c_custkey,
            o_orderkey,
            o_orderdate,
            o_totalprice
        order by
            o_totalprice desc,
            o_orderdate
        limit 100
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[2].i32(), 6);
    EXPECT_NEAR(r.result.rows[0]->values_[5].to_f64(), 350.0, 1e-6);
}

TEST_F(TpchQueryTest, Q19_DiscountedRevenue)
{
    auto r = must(R"(
        select
            sum(l_extendedprice* (1 - l_discount)) as revenue
        from
            lineitem,
            part
        where
            (
                p_partkey = l_partkey
                and p_brand = 'Brand#12'
                and p_container in ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
                and l_quantity >= 1 and l_quantity <= 1 + 10
                and p_size between 1 and 5
                and l_shipmode in ('AIR', 'AIR REG')
                and l_shipinstruct = 'DELIVER IN PERSON'
            )
            or
            (
                p_partkey = l_partkey
                and p_brand = 'Brand#23'
                and p_container in ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
                and l_quantity >= 10 and l_quantity <= 10 + 10
                and p_size between 1 and 10
                and l_shipmode in ('AIR', 'AIR REG')
                and l_shipinstruct = 'DELIVER IN PERSON'
            )
            or
            (
                p_partkey = l_partkey
                and p_brand = 'Brand#34'
                and p_container in ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
                and l_quantity >= 20 and l_quantity <= 20 + 10
                and p_size between 1 and 15
                and l_shipmode in ('AIR', 'AIR REG')
                and l_shipinstruct = 'DELIVER IN PERSON'
            )
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_NEAR(r.result.rows[0]->values_[0].to_f64(), 900.0, 1e-6);
}

TEST_F(TpchQueryTest, Q20_PotentialPartPromotion)
{
    auto r = must(R"(
        select
            s_name,
            s_address
        from
            supplier,
            nation
        where
            s_suppkey in (
                select
                    ps_suppkey
                from
                    partsupp
                where
                    ps_partkey in (
                        select
                            p_partkey
                        from
                            part
                        where
                            p_name like 'forest%'
                    )
                    and ps_availqty > (
                        select
                            0.5 * sum(l_quantity)
                        from
                            lineitem
                        where
                            l_partkey = ps_partkey
                            and l_suppkey = ps_suppkey
                            and l_shipdate >= date '1994-01-01'
                            and l_shipdate < date '1994-01-01' + interval '1' year
                    )
            )
            and s_nationkey = n_nationkey
            and n_name = 'CANADA'
        order by
            s_name
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Supplier#000000004");
}

TEST_F(TpchQueryTest, Q21_SuppliersWhoKeptOrdersWaiting)
{
    auto r = must(R"(
        select
            s_name,
            count(*) as numwait
        from
            supplier,
            lineitem l1,
            orders,
            nation
        where
            s_suppkey = l1.l_suppkey
            and o_orderkey = l1.l_orderkey
            and o_orderstatus = 'F'
            and l1.l_receiptdate > l1.l_commitdate
            and exists (
                select
                    *
                from
                    lineitem l2
                where
                    l2.l_orderkey = l1.l_orderkey
                    and l2.l_suppkey <> l1.l_suppkey
            )
            and not exists (
                select
                    *
                from
                    lineitem l3
                where
                    l3.l_orderkey = l1.l_orderkey
                    and l3.l_suppkey <> l1.l_suppkey
                    and l3.l_receiptdate > l3.l_commitdate
            )
            and s_nationkey = n_nationkey
            and n_name = 'SAUDI ARABIA'
        group by
            s_name
        order by
            numwait desc,
            s_name
        limit 100
    )");
    ASSERT_EQ(r.result.rows.size(), 1u);
    EXPECT_EQ(r.result.rows[0]->values_[0].string_ref(), "Supplier#000000006");
    EXPECT_EQ(r.result.rows[0]->values_[1].i64(), 1);
}

TEST_F(TpchQueryTest, Q22_GlobalSalesOpportunity)
{
    auto r = must(R"(
        select
            cntrycode,
            count(*) as numcust,
            sum(c_acctbal) as totacctbal
        from (
            select
                substring(c_phone from 1 for 2) as cntrycode,
                c_acctbal
            from
                customer
            where
                substring(c_phone from 1 for 2) in
                    ('13', '31', '23', '29', '30', '18', '17')
                and c_acctbal > (
                    select
                        avg(c_acctbal)
                    from
                        customer
                    where
                        c_acctbal > 0.00
                        and substring(c_phone from 1 for 2) in
                            ('13', '31', '23', '29', '30', '18', '17')
                )
                and not exists (
                    select
                        *
                    from
                        orders
                    where
                        o_custkey = c_custkey
                )
        ) as custsale
        group by
            cntrycode
        order by
            cntrycode
    )");
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

} // namespace
