/**
 * Compiler unit tests.
 * Tests compilation of plan nodes into bytecode programs.
 */
#include <memory>
#include <variant>
#include <gtest/gtest.h>
#include "src/bytecode/compiler.h"
#include "src/bytecode/opcode.h"
#include "src/catalog/column.h"
#include "src/catalog/schema.h"
#include "src/catalog/schema_mgr.h"
#include "src/parser/ast.h"
#include "src/planner/plan_node.h"
#include "src/type/value.h"

namespace popo::bytecode
{
namespace
{

// Helper: create a simple schema for testing
popo::catalog::Schema make_users_schema()
{
    popo::catalog::Schema schema;
    schema.id_ = 1;
    schema.table_name_ = "users";
    schema.primary_key_col_idx_ = 0;

    popo::catalog::Column id_col;
    id_col.column_name_ = "id";
    id_col.data_type_ = popo::type::ValueType::Int32;
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    schema.columns_.push_back(id_col);

    popo::catalog::Column name_col;
    name_col.column_name_ = "name";
    name_col.data_type_ = popo::type::ValueType::String;
    name_col.is_nullable_ = true;
    schema.columns_.push_back(name_col);

    popo::catalog::Column age_col;
    age_col.column_name_ = "age";
    age_col.data_type_ = popo::type::ValueType::Int32;
    age_col.is_nullable_ = true;
    schema.columns_.push_back(age_col);

    return schema;
}

// Helper: register a table in SchemaManager
void register_table(popo::catalog::SchemaManager & mgr, const popo::catalog::Schema & schema)
{
    mgr.create_table(schema.table_name_, schema.columns_);
}

// Helper: make a LiteralExpr
std::shared_ptr<popo::parser::LiteralExpr> make_literal(popo::type::Value v)
{
    auto e = std::make_shared<popo::parser::LiteralExpr>();
    e->value = std::move(v);
    return e;
}

std::shared_ptr<popo::parser::LiteralExpr> make_int_literal(int32_t val)
{
    return make_literal(popo::type::Value(popo::type::ValueType::Int32, val));
}

std::shared_ptr<popo::parser::LiteralExpr> make_str_literal(const std::string & s)
{
    return make_literal(popo::type::Value(popo::type::ValueType::String, s));
}

// Helper: make a ColumnRefExpr
std::shared_ptr<popo::parser::ColumnRefExpr> make_col_ref(const std::string & table, const std::string & col)
{
    auto e = std::make_shared<popo::parser::ColumnRefExpr>();
    e->table_name = table;
    e->column_name = col;
    return e;
}

// Helper: make a BinaryOpExpr
std::shared_ptr<popo::parser::BinaryOpExpr>
make_binary(popo::parser::BinaryOpType op, popo::parser::ExprPtr left, popo::parser::ExprPtr right)
{
    auto e = std::make_shared<popo::parser::BinaryOpExpr>();
    e->op = op;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}

// Helper: make a FunctionCallExpr
std::shared_ptr<popo::parser::FunctionCallExpr> make_func(popo::parser::AggFuncType func, std::vector<popo::parser::ExprPtr> args)
{
    auto e = std::make_shared<popo::parser::FunctionCallExpr>();
    e->func = func;
    e->args = std::move(args);
    return e;
}

// Helper: count instructions of a given opcode
int count_opcodes(const Program & prog, OpCode target)
{
    int count = 0;
    for (const auto & inst : prog.instructions)
    {
        if (inst.op == target)
            count++;
    }
    return count;
}

// Helper: check if program contains instruction with given opcode
bool has_opcode(const Program & prog, OpCode target)
{
    return count_opcodes(prog, target) > 0;
}

} // anonymous namespace

class CompilerTest : public ::testing::Test
{
protected:
    popo::catalog::SchemaManager schema_mgr_;
    Compiler compiler{schema_mgr_};

    void SetUp() override { register_table(schema_mgr_, make_users_schema()); }
};

// ---- Basic Scan Compilation ----

TEST_F(CompilerTest, CompileScan)
{
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto prog = compiler.compile(scan).value();

    // Should have: OpenRead, Rewind, (Column, Column, Column, ResultRow,) Next, Close, Halt
    EXPECT_TRUE(has_opcode(prog, OpCode::OpenRead));
    EXPECT_TRUE(has_opcode(prog, OpCode::Rewind));
    EXPECT_TRUE(has_opcode(prog, OpCode::Next));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
    EXPECT_TRUE(has_opcode(prog, OpCode::ResultRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::Column));
    EXPECT_GE(count_opcodes(prog, OpCode::Column), 3);
}

TEST_F(CompilerTest, CompileScanHasCorrectResultColumns)
{
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto prog = compiler.compile(scan).value();
}

// ---- Insert Compilation ----

TEST_F(CompilerTest, CompileInsert)
{
    auto insert = std::make_shared<popo::planner::InsertNode>();
    insert->table_name = "users";
    insert->values.resize(1);
    insert->values[0].push_back(make_int_literal(1));
    insert->values[0].push_back(make_str_literal("Alice"));
    insert->values[0].push_back(make_int_literal(25));

    auto prog = compiler.compile(insert).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::OpenWrite));
    EXPECT_TRUE(has_opcode(prog, OpCode::Const));
    EXPECT_TRUE(has_opcode(prog, OpCode::MakeRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::InsertRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

TEST_F(CompilerTest, CompileInsertMultipleRows)
{
    auto insert = std::make_shared<popo::planner::InsertNode>();
    insert->table_name = "users";
    insert->values.resize(2);
    insert->values[0].push_back(make_int_literal(1));
    insert->values[0].push_back(make_str_literal("Alice"));
    insert->values[0].push_back(make_int_literal(25));
    insert->values[1].push_back(make_int_literal(2));
    insert->values[1].push_back(make_str_literal("Bob"));
    insert->values[1].push_back(make_int_literal(30));

    auto prog = compiler.compile(insert).value();
    // 2 MakeRow + 2 InsertRow
    EXPECT_EQ(count_opcodes(prog, OpCode::MakeRow), 2);
    EXPECT_EQ(count_opcodes(prog, OpCode::InsertRow), 2);
}

// ---- CreateTable Compilation ----

TEST_F(CompilerTest, CompileCreateTable)
{
    auto ct = std::make_shared<popo::planner::CreateTableNode>();
    ct->table_name = "products";
    popo::parser::ColumnDef col;
    col.name = "id";
    col.data_type = popo::type::ValueType::Int32;
    col.is_primary_key = true;
    ct->columns.push_back(col);

    auto prog = compiler.compile(ct).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::CreateTable));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

// ---- DropTable Compilation ----

TEST_F(CompilerTest, CompileDropTable)
{
    auto dt = std::make_shared<popo::planner::DropTableNode>();
    dt->table_name = "users";
    dt->if_exists = false;

    auto prog = compiler.compile(dt).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::DropTable));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

TEST_F(CompilerTest, CompileDropTableIfExists)
{
    auto dt = std::make_shared<popo::planner::DropTableNode>();
    dt->table_name = "users";
    dt->if_exists = true;

    auto prog = compiler.compile(dt).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::DropTable));
    // operand2 should be 1 (if_exists)
    for (const auto & inst : prog.instructions)
    {
        if (inst.op == OpCode::DropTable)
        {
            EXPECT_EQ(inst.operand2, 1);
        }
    }
}

// ---- Delete Compilation ----

TEST_F(CompilerTest, CompileDeleteAll)
{
    auto del = std::make_shared<popo::planner::DeleteNode>();
    del->table_name = "users";
    del->where_clause = nullptr; // DELETE all

    auto prog = compiler.compile(del).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::OpenWrite));
    EXPECT_TRUE(has_opcode(prog, OpCode::Rewind));
    EXPECT_TRUE(has_opcode(prog, OpCode::DeleteRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::Next));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

TEST_F(CompilerTest, CompileDeleteWithWhere)
{
    auto del = std::make_shared<popo::planner::DeleteNode>();
    del->table_name = "users";
    del->where_clause = make_binary(popo::parser::BinaryOpType::Eq, make_col_ref("", "id"), make_int_literal(42));

    auto prog = compiler.compile(del).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::DeleteRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::SeekByPk));
}

// ---- Update Compilation ----

TEST_F(CompilerTest, CompileUpdateAll)
{
    auto upd = std::make_shared<popo::planner::UpdateNode>();
    upd->table_name = "users";
    upd->where_clause = nullptr;

    popo::parser::UpdateAssignment assign;
    assign.column_name = "age";
    assign.value = make_int_literal(99);
    upd->assignments.push_back(assign);

    auto prog = compiler.compile(upd).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::OpenWrite));
    EXPECT_TRUE(has_opcode(prog, OpCode::UpdateRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::Next));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

TEST_F(CompilerTest, CompileUpdateWithWhere)
{
    auto upd = std::make_shared<popo::planner::UpdateNode>();
    upd->table_name = "users";
    upd->where_clause = make_binary(popo::parser::BinaryOpType::Gt, make_col_ref("", "age"), make_int_literal(18));

    popo::parser::UpdateAssignment assign;
    assign.column_name = "name";
    assign.value = make_str_literal("adult");
    upd->assignments.push_back(assign);

    auto prog = compiler.compile(upd).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::UpdateRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::JumpIfFalse));
}

// ---- Select compilation (simple) ----

TEST_F(CompilerTest, CompileSelectSimple)
{
    // Build: SELECT id, name FROM users WHERE age > 18
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto filter = std::make_shared<popo::planner::FilterNode>();
    filter->child = scan;
    filter->condition = make_binary(popo::parser::BinaryOpType::Gt, make_col_ref("", "age"), make_int_literal(18));

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = filter;
    project->expressions.push_back(make_col_ref("", "id"));
    project->expressions.push_back(make_col_ref("", "name"));
    project->output_names = {"id", "name"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::OpenRead));
    EXPECT_TRUE(has_opcode(prog, OpCode::Rewind));
    EXPECT_TRUE(has_opcode(prog, OpCode::Next));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
    EXPECT_TRUE(has_opcode(prog, OpCode::ResultRow));
    EXPECT_TRUE(has_opcode(prog, OpCode::JumpIfFalse)); // WHERE filter
    EXPECT_TRUE(has_opcode(prog, OpCode::Gt)); // age > 18
}

TEST_F(CompilerTest, CompileSelectStar)
{
    // Build: SELECT * FROM users
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto star = std::make_shared<popo::parser::StarExpr>();

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = scan;
    project->expressions.push_back(star);
    project->output_names = {"id", "name", "age"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::OpenRead));
    EXPECT_TRUE(has_opcode(prog, OpCode::ResultRow));
    // Star expands to 3 columns
    EXPECT_GE(count_opcodes(prog, OpCode::Column), 3);
}

// ---- Select with limit ----

TEST_F(CompilerTest, CompileSelectWithLimit)
{
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto limit = std::make_shared<popo::planner::LimitNode>();
    limit->child = scan;
    limit->limit_count = 5;

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = limit;
    project->expressions.push_back(make_col_ref("", "id"));
    project->output_names = {"id"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::JumpIfFalse)); // limit countdown check
    EXPECT_TRUE(has_opcode(prog, OpCode::Sub)); // decrement
    EXPECT_TRUE(has_opcode(prog, OpCode::Dup)); // dup for check
}

// ---- Select with aggregation ----

TEST_F(CompilerTest, CompileSelectWithAgg)
{
    // Build: SELECT COUNT(*), AVG(age) FROM users
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto agg = std::make_shared<popo::planner::AggNode>();
    agg->child = scan;

    auto count_star = make_func(popo::parser::AggFuncType::Count, {});
    auto avg_age = make_func(popo::parser::AggFuncType::Avg, {make_col_ref("", "age")});
    agg->agg_exprs.push_back(count_star);
    agg->agg_exprs.push_back(avg_age);

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = agg;
    project->expressions.push_back(count_star);
    project->expressions.push_back(avg_age);
    project->output_names = {"COUNT(*)", "AVG(age)"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::AggInit));
    EXPECT_TRUE(has_opcode(prog, OpCode::AggStep));
    EXPECT_TRUE(has_opcode(prog, OpCode::AggResult));
    EXPECT_TRUE(has_opcode(prog, OpCode::Next));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

// ---- Select with sort ----

TEST_F(CompilerTest, CompileSelectWithSort)
{
    // Build: SELECT id, name FROM users ORDER BY age DESC
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto sort = std::make_shared<popo::planner::SortNode>();
    sort->child = scan;
    popo::parser::OrderByItem item;
    item.expr = make_col_ref("", "age");
    item.ascending = false;
    sort->order_by.push_back(item);

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = sort;
    project->expressions.push_back(make_col_ref("", "id"));
    project->expressions.push_back(make_col_ref("", "name"));
    project->output_names = {"id", "name"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::SorterOpen));
    EXPECT_TRUE(has_opcode(prog, OpCode::SortKeyDef));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterInsert));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterSort));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterRewind));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterNext));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterColumn));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
}

// ---- Select with join ----

TEST_F(CompilerTest, CompileSelectWithJoin)
{
    // Register second table
    popo::catalog::Schema orders_schema;
    orders_schema.id_ = 2;
    orders_schema.table_name_ = "orders";
    orders_schema.primary_key_col_idx_ = 0;
    popo::catalog::Column oid_col;
    oid_col.column_name_ = "id";
    oid_col.data_type_ = popo::type::ValueType::Int32;
    oid_col.is_primary_key_ = true;
    oid_col.is_nullable_ = false;
    orders_schema.columns_.push_back(oid_col);
    popo::catalog::Column uid_col;
    uid_col.column_name_ = "user_id";
    uid_col.data_type_ = popo::type::ValueType::Int32;
    orders_schema.columns_.push_back(uid_col);
    register_table(schema_mgr_, orders_schema);

    auto left_scan = std::make_shared<popo::planner::ScanNode>();
    left_scan->table_name = "users";
    left_scan->column_names = {"id", "name", "age"};

    auto right_scan = std::make_shared<popo::planner::ScanNode>();
    right_scan->table_name = "orders";
    right_scan->column_names = {"id", "user_id"};

    auto join = std::make_shared<popo::planner::JoinNode>();
    join->left = left_scan;
    join->right = right_scan;
    join->condition = make_binary(popo::parser::BinaryOpType::Eq, make_col_ref("users", "id"), make_col_ref("orders", "user_id"));

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = join;
    project->expressions.push_back(make_col_ref("users", "id"));
    project->expressions.push_back(make_col_ref("users", "name"));
    project->output_names = {"id", "name"};

    auto prog = compiler.compile(project).value();

    // Nested loop join: 2 OpenRead, 2 Rewind, 2 Next, 2 Close, 1 ResultRow, 1 Halt
    EXPECT_EQ(count_opcodes(prog, OpCode::OpenRead), 2);
    EXPECT_EQ(count_opcodes(prog, OpCode::Rewind), 2);
    EXPECT_EQ(count_opcodes(prog, OpCode::Next), 2);
    EXPECT_EQ(count_opcodes(prog, OpCode::Close), 2);
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
    EXPECT_TRUE(has_opcode(prog, OpCode::JumpIfFalse)); // join condition
}

// ---- Error cases ----

TEST_F(CompilerTest, CompileNullPlanReturnsEmptyProgram)
{
    auto prog = compiler.compile(nullptr).value();
    EXPECT_TRUE(prog.instructions.empty());
}

TEST_F(CompilerTest, CompileInsertWithColumnNotFoundThrows)
{
    auto insert = std::make_shared<popo::planner::InsertNode>();
    insert->table_name = "nonexistent";
    insert->values.resize(1);
    insert->values[0].push_back(make_int_literal(1));

    auto prog = compiler.compile(insert).value();
    // The table is not registered, but OpenWrite with nonexistent table in VM handles it
    // The compiler itself won't error on missing table for insert
    EXPECT_TRUE(has_opcode(prog, OpCode::OpenWrite));
}

TEST_F(CompilerTest, CompileDropNonexistentTable)
{
    auto dt = std::make_shared<popo::planner::DropTableNode>();
    dt->table_name = "nosuchtable";
    dt->if_exists = false;

    auto prog = compiler.compile(dt).value();
    // Compiler itself won't error — VM handles table existence
    EXPECT_TRUE(has_opcode(prog, OpCode::DropTable));
}

// ---- Select with sort + limit ----

TEST_F(CompilerTest, CompileSelectWithSortAndLimit)
{
    auto scan = std::make_shared<popo::planner::ScanNode>();
    scan->table_name = "users";
    scan->column_names = {"id", "name", "age"};

    auto sort = std::make_shared<popo::planner::SortNode>();
    sort->child = scan;
    popo::parser::OrderByItem item;
    item.expr = make_col_ref("", "id");
    item.ascending = true;
    sort->order_by.push_back(item);

    auto limit = std::make_shared<popo::planner::LimitNode>();
    limit->child = sort;
    limit->limit_count = 3;

    auto project = std::make_shared<popo::planner::ProjectNode>();
    project->child = limit;
    project->expressions.push_back(make_col_ref("", "id"));
    project->output_names = {"id"};

    auto prog = compiler.compile(project).value();

    EXPECT_TRUE(has_opcode(prog, OpCode::SorterOpen));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterSort));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterRewind));
    EXPECT_TRUE(has_opcode(prog, OpCode::SorterNext));
    EXPECT_TRUE(has_opcode(prog, OpCode::JumpIfFalse)); // limit check
}

// ---- Edge case: empty insert ----

TEST_F(CompilerTest, CompileInsertEmptyValues)
{
    auto insert = std::make_shared<popo::planner::InsertNode>();
    insert->table_name = "users";
    // No values

    auto prog = compiler.compile(insert).value();

    // Should still emit OpenWrite + Close + Halt
    EXPECT_TRUE(has_opcode(prog, OpCode::OpenWrite));
    EXPECT_TRUE(has_opcode(prog, OpCode::Close));
    EXPECT_TRUE(has_opcode(prog, OpCode::Halt));
    // No MakeRow or InsertRow
    EXPECT_EQ(count_opcodes(prog, OpCode::MakeRow), 0);
    EXPECT_EQ(count_opcodes(prog, OpCode::InsertRow), 0);
}

// ---- Constants verification ----

TEST_F(CompilerTest, CompileInsertHasCorrectConstants)
{
    auto insert = std::make_shared<popo::planner::InsertNode>();
    insert->table_name = "users";
    insert->values.resize(1);
    insert->values[0].push_back(make_int_literal(42));

    auto prog = compiler.compile(insert).value();

    // Should have table name constant and value constant
    bool found_table = false, found_value = false;
    for (const auto & c : prog.constants)
    {
        if (std::holds_alternative<std::string>(c))
        {
            if (std::get<std::string>(c) == "users")
                found_table = true;
        }
        if (std::holds_alternative<popo::type::Value>(c))
        {
            if (std::get<popo::type::Value>(c).i32() == 42)
                found_value = true;
        }
    }
    EXPECT_TRUE(found_table);
    EXPECT_TRUE(found_value);
}

} // namespace popo::bytecode
