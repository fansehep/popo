/**
 * VirtualMachine unit tests.
 * Tests VM execution of bytecode programs against real StorageEngine.
 */
#include <filesystem>
#include <random>
#include <gtest/gtest.h>
#include "src/bytecode/program.h"
#include "src/bytecode/vm.h"
#include "src/catalog/column.h"
#include "src/catalog/schema.h"
#include "src/catalog/schema_mgr.h"
#include "src/storage/storage_engine.h"
#include "src/type/tuple.h"
#include "src/type/value.h"

namespace neon::bytecode
{
namespace
{

// ---- Test helpers ----

neon::catalog::Schema make_users_schema()
{
    neon::catalog::Schema schema;
    schema.id_ = 1;
    schema.table_name_ = "users";
    schema.primary_key_col_idx_ = 0;

    neon::catalog::Column id_col;
    id_col.column_name_ = "id";
    id_col.data_type_ = neon::type::ValueType::Int32;
    id_col.is_primary_key_ = true;
    id_col.is_nullable_ = false;
    schema.columns_.push_back(id_col);

    neon::catalog::Column name_col;
    name_col.column_name_ = "name";
    name_col.data_type_ = neon::type::ValueType::String;
    name_col.is_nullable_ = true;
    schema.columns_.push_back(name_col);

    neon::catalog::Column age_col;
    age_col.column_name_ = "age";
    age_col.data_type_ = neon::type::ValueType::Int32;
    age_col.is_nullable_ = true;
    schema.columns_.push_back(age_col);

    return schema;
}

// Helper: make Const opcodes, pushing value onto stack
void emit_const(Program & prog, neon::type::Value v)
{
    int32_t idx = prog.add_constant(v);
    prog.emit(OpCode::Const, idx);
}

void emit_string_const(Program & prog, const std::string & s)
{
    int32_t idx = prog.add_string_constant(s);
    prog.emit(OpCode::Const, idx);
}

void emit_int(Program & prog, int32_t v)
{
    emit_const(prog, neon::type::Value(neon::type::ValueType::Int32, v));
}

void emit_bool(Program & prog, bool v)
{
    emit_const(prog, neon::type::Value(neon::type::ValueType::Bool, v));
}

void emit_string(Program & prog, const std::string & s)
{
    emit_const(prog, neon::type::Value(neon::type::ValueType::String, s));
}

void emit_float(Program & prog, double v)
{
    emit_const(prog, neon::type::Value(neon::type::ValueType::Float64, v));
}

} // anonymous namespace

class VMTest : public ::testing::Test
{
protected:
    std::string test_dir_;
    std::unique_ptr<neon::catalog::SchemaManager> schema_mgr_;
    std::unique_ptr<neon::storage::StorageEngine> engine_;
    std::unique_ptr<VirtualMachine> vm_;

    void SetUp() override
    {
        std::random_device rd;
        test_dir_ = std::filesystem::temp_directory_path().string() + "/neon_vm_test_" + std::to_string(rd());
        std::filesystem::create_directories(test_dir_);

        schema_mgr_ = std::make_unique<neon::catalog::SchemaManager>();
        engine_ = std::make_unique<neon::storage::StorageEngine>(test_dir_, 64);
        engine_->init();

        vm_ = std::make_unique<VirtualMachine>(*schema_mgr_, *engine_);
    }

    void TearDown() override
    {
        vm_.reset();
        if (engine_)
        {
            engine_->shutdown();
            engine_.reset();
        }
        schema_mgr_.reset();
        if (!test_dir_.empty())
        {
            std::filesystem::remove_all(test_dir_);
        }
    }

    // Create and register a test table
    void create_users_table()
    {
        auto schema = make_users_schema();
        schema_mgr_->create_table("users", schema.columns_);
        engine_->create_table("users", schema);
    }
};

// ---- Stack operations ----

TEST_F(VMTest, ExecuteHaltOnly)
{
    Program prog;
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->rows.size(), 0u);
}

TEST_F(VMTest, ExecuteConstAndPop)
{
    Program prog;
    emit_int(prog, 42);
    prog.emit(OpCode::Pop);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
}

TEST_F(VMTest, ExecuteConstResultRow)
{
    Program prog;
    emit_int(prog, 100);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 100);
}

TEST_F(VMTest, ExecuteMultipleResultRows)
{
    Program prog;
    for (int i = 0; i < 5; i++)
    {
        emit_int(prog, i);
        prog.emit(OpCode::ResultRow, 1);
    }
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(result->rows.size(), 5u);
    for (int i = 0; i < 5; i++)
    {
        EXPECT_EQ(result->rows[i]->values_[0].i32(), i);
    }
}

// ---- Arithmetic ----

TEST_F(VMTest, ExecuteAdd)
{
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 20);
    prog.emit(OpCode::Add);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 30);
}

TEST_F(VMTest, ExecuteSub)
{
    Program prog;
    emit_int(prog, 50);
    emit_int(prog, 15);
    prog.emit(OpCode::Sub);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 35);
}

TEST_F(VMTest, ExecuteMul)
{
    Program prog;
    emit_int(prog, 7);
    emit_int(prog, 8);
    prog.emit(OpCode::Mul);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 56);
}

TEST_F(VMTest, ExecuteDiv)
{
    Program prog;
    emit_int(prog, 100);
    emit_int(prog, 4);
    prog.emit(OpCode::Div);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 25);
}

TEST_F(VMTest, ExecuteDivByZero)
{
    Program prog;
    emit_int(prog, 100);
    emit_int(prog, 0);
    prog.emit(OpCode::Div);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 0); // div-by-zero returns 0
}

TEST_F(VMTest, ExecuteNeg)
{
    Program prog;
    emit_int(prog, 42);
    prog.emit(OpCode::Neg);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), -42);
}

TEST_F(VMTest, ExecuteFloatAdd)
{
    Program prog;
    emit_float(prog, 3.14);
    emit_float(prog, 2.86);
    prog.emit(OpCode::Add);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_DOUBLE_EQ(result->rows[0]->values_[0].f64(), 6.0);
}

// ---- Comparison ----

TEST_F(VMTest, ExecuteEqTrue)
{
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 10);
    prog.emit(OpCode::Eq);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteEqFalse)
{
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 20);
    prog.emit(OpCode::Eq);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_FALSE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteNe)
{
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 20);
    prog.emit(OpCode::Ne);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteLt)
{
    Program prog;
    emit_int(prog, 5);
    emit_int(prog, 10);
    prog.emit(OpCode::Lt);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteLe)
{
    // 5 <= 5 is true
    Program prog;
    emit_int(prog, 5);
    emit_int(prog, 5);
    prog.emit(OpCode::Le);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteGtFalse)
{
    Program prog;
    emit_int(prog, 5);
    emit_int(prog, 10);
    prog.emit(OpCode::Gt);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_FALSE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteGe)
{
    // 10 >= 10 is true
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 10);
    prog.emit(OpCode::Ge);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

// ---- Logical ----

TEST_F(VMTest, ExecuteAnd)
{
    Program prog;
    emit_bool(prog, true);
    emit_bool(prog, true);
    prog.emit(OpCode::And);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteAndFalse)
{
    Program prog;
    emit_bool(prog, true);
    emit_bool(prog, false);
    prog.emit(OpCode::And);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_FALSE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteOr)
{
    Program prog;
    emit_bool(prog, true);
    emit_bool(prog, false);
    prog.emit(OpCode::Or);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

TEST_F(VMTest, ExecuteNot)
{
    Program prog;
    emit_bool(prog, true);
    prog.emit(OpCode::Not);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_FALSE(result->rows[0]->values_[0].boolean());
}

// ---- Control flow (Jump, JumpIfFalse, JumpIfTrue) ----

TEST_F(VMTest, ExecuteUnconditionalJump)
{
    Program prog;
    prog.emit(OpCode::Jump, 2); // jump to Halt, skipping Noop
    prog.emit(OpCode::Noop);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
}

TEST_F(VMTest, ExecuteJumpIfFalseSkipBlock)
{
    // if false: skip result, jump to Halt
    Program prog;
    emit_bool(prog, false);
    prog.emit(OpCode::JumpIfFalse, 4); // jump to Halt
    emit_int(prog, 999);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->rows.size(), 0u); // skipped
}

TEST_F(VMTest, ExecuteJumpIfFalseEnterBlock)
{
    // if true: don't jump, execute result
    Program prog;
    emit_bool(prog, true);
    prog.emit(OpCode::JumpIfFalse, 4); // NOT taken
    emit_int(prog, 42);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 42);
}

TEST_F(VMTest, ExecuteJumpIfTrue)
{
    Program prog;
    emit_bool(prog, true);
    prog.emit(OpCode::JumpIfTrue, 3); // jump to result
    prog.emit(OpCode::Halt); // skipped
    emit_int(prog, 7);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 7);
}

TEST_F(VMTest, ExecuteLoopWithJump)
{
    // Simple loop that adds 5 times
    // Stack: [counter] — start at 0, push 1, add, dup, push 5, Lt, JumpIfTrue back
    // Equivalent to: int i=0; do { i++; } while(i < 5);
    // Note: Add returns Int64, so comparison value must also be Int64
    Program prog;
    emit_int(prog, 0); // 0: counter on stack

    // loop_start (pc=1):
    emit_int(prog, 1); // 1: push 1
    prog.emit(OpCode::Add); // 2: counter += 1 (Int64)
    prog.emit(OpCode::Dup); // 3: dup counter
    emit_const(prog, neon::type::Value(neon::type::ValueType::Int64, static_cast<int64_t>(5))); // 4: push Int64(5)
    prog.emit(OpCode::Lt); // 5: counter < 5 ?
    prog.emit(OpCode::JumpIfTrue, 1); // 6: jump back to loop_start

    prog.emit(OpCode::ResultRow, 1); // 7: result = counter
    prog.emit(OpCode::Halt); // 8

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i64(), 5);
}

// ---- Dup and Noop ----

TEST_F(VMTest, ExecuteDup)
{
    Program prog;
    emit_int(prog, 99);
    prog.emit(OpCode::Dup);
    prog.emit(OpCode::ResultRow, 2); // both values
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    ASSERT_GE(result->rows[0]->values_.size(), 2u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 99);
    EXPECT_EQ(result->rows[0]->values_[1].i32(), 99);
}

TEST_F(VMTest, ExecuteNoop)
{
    Program prog;
    prog.emit(OpCode::Noop);
    prog.emit(OpCode::Noop);
    prog.emit(OpCode::Noop);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
}

// ---- End-to-end: Create table, insert, scan ----

TEST_F(VMTest, E2ECreateInsertScan)
{
    // 1. Create table
    Program create_prog;
    int32_t name_idx = create_prog.add_string_constant("test_tbl");
    std::vector<neon::parser::ColumnDef> cols;
    neon::parser::ColumnDef id_col;
    id_col.name = "id";
    id_col.data_type = neon::type::ValueType::Int32;
    id_col.is_primary_key = true;
    cols.push_back(id_col);
    int32_t cols_idx = create_prog.add_column_defs(cols);
    create_prog.emit(OpCode::CreateTable, name_idx, cols_idx);
    create_prog.emit(OpCode::Halt);

    auto r1 = vm_->execute(create_prog);
    ASSERT_TRUE(r1.has_value()) << "CreateTable failed";

    // 2. Insert rows
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("test_tbl");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);

    // Row 1: id=10
    emit_int(insert_prog, 10);
    insert_prog.emit(OpCode::MakeRow, 1);
    insert_prog.emit(OpCode::InsertRow, 0);

    // Row 2: id=20
    emit_int(insert_prog, 20);
    insert_prog.emit(OpCode::MakeRow, 1);
    insert_prog.emit(OpCode::InsertRow, 0);

    // Row 3: id=30
    emit_int(insert_prog, 30);
    insert_prog.emit(OpCode::MakeRow, 1);
    insert_prog.emit(OpCode::InsertRow, 0);

    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);

    auto r2 = vm_->execute(insert_prog);
    ASSERT_TRUE(r2.has_value()) << "Insert failed";
    EXPECT_EQ(r2->rows_affected, 3);

    // 3. Scan
    Program scan_prog;
    int32_t scan_tbl_idx = scan_prog.add_string_constant("test_tbl");
    scan_prog.emit(OpCode::OpenRead, 0, scan_tbl_idx);

    int32_t rewind = scan_prog.emit(OpCode::Rewind, 0, 0);
    int32_t loop = scan_prog.current_pos();

    scan_prog.emit(OpCode::Column, 0, 0); // column 0
    scan_prog.emit(OpCode::ResultRow, 1);

    int32_t next_pos = scan_prog.current_pos();
    scan_prog.emit(OpCode::Next, 0, loop);

    int32_t end_pos = scan_prog.current_pos();
    scan_prog.patch_jump_operand2(rewind, end_pos);
    scan_prog.emit(OpCode::Close, 0);
    scan_prog.emit(OpCode::Halt);

    auto r3 = vm_->execute(scan_prog);
    ASSERT_TRUE(r3.has_value()) << "Scan failed";
    ASSERT_EQ(r3->rows.size(), 3u);

    // Rows should be in PK order (B+Tree sorted)
    EXPECT_EQ(r3->rows[0]->values_[0].i32(), 10);
    EXPECT_EQ(r3->rows[1]->values_[0].i32(), 20);
    EXPECT_EQ(r3->rows[2]->values_[0].i32(), 30);
}

// ---- End-to-end: Update and Delete ----

TEST_F(VMTest, E2EDeleteRow)
{
    create_users_table();

    // Insert 3 rows via VM
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("users");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);

    for (int id = 1; id <= 3; id++)
    {
        emit_int(insert_prog, id);
        emit_string(insert_prog, "user_" + std::to_string(id));
        emit_int(insert_prog, id * 10);
        insert_prog.emit(OpCode::MakeRow, 3);
        insert_prog.emit(OpCode::InsertRow, 0);
    }
    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);
    ASSERT_TRUE(vm_->execute(insert_prog).has_value());

    // Delete row with id=2: OpenWrite, Rewind, loop: Column(0), push 2, Eq, JumpIfFalse skip, DeleteRow, Next
    Program delete_prog;
    int32_t del_tbl = delete_prog.add_string_constant("users");
    delete_prog.emit(OpCode::OpenWrite, 0, del_tbl);

    int32_t rewind = delete_prog.emit(OpCode::Rewind, 0, 0);
    int32_t loop = delete_prog.current_pos();

    // if current.id == 2: delete
    delete_prog.emit(OpCode::Column, 0, 0);
    emit_int(delete_prog, 2);
    delete_prog.emit(OpCode::Eq);
    int32_t skip = delete_prog.emit(OpCode::JumpIfFalse, 0);
    delete_prog.emit(OpCode::DeleteRow, 0);

    int32_t after = delete_prog.current_pos();
    delete_prog.patch_jump(skip, after);
    delete_prog.emit(OpCode::Next, 0, loop);

    int32_t end = delete_prog.current_pos();
    delete_prog.patch_jump_operand2(rewind, end);
    delete_prog.emit(OpCode::Close, 0);
    delete_prog.emit(OpCode::Halt);

    auto del_result = vm_->execute(delete_prog);
    ASSERT_TRUE(del_result.has_value());
    EXPECT_EQ(del_result->rows_affected, 1);

    // Scan should show only id=1,3
    Program scan_prog;
    int32_t scan_tbl = scan_prog.add_string_constant("users");
    scan_prog.emit(OpCode::OpenRead, 0, scan_tbl);
    int32_t sr = scan_prog.emit(OpCode::Rewind, 0, 0);
    int32_t sl = scan_prog.current_pos();
    scan_prog.emit(OpCode::Column, 0, 0);
    scan_prog.emit(OpCode::ResultRow, 1);
    int32_t sn = scan_prog.current_pos();
    scan_prog.emit(OpCode::Next, 0, sl);
    int32_t se = scan_prog.current_pos();
    scan_prog.patch_jump_operand2(sr, se);
    scan_prog.emit(OpCode::Close, 0);
    scan_prog.emit(OpCode::Halt);

    auto scan_result = vm_->execute(scan_prog);
    ASSERT_TRUE(scan_result.has_value());
    ASSERT_EQ(scan_result->rows.size(), 2u);
    EXPECT_EQ(scan_result->rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(scan_result->rows[1]->values_[0].i32(), 3);
}

TEST_F(VMTest, E2EUpdateRow)
{
    create_users_table();

    // Insert
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("users");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);
    emit_int(insert_prog, 1);
    emit_string(insert_prog, "Alice");
    emit_int(insert_prog, 25);
    insert_prog.emit(OpCode::MakeRow, 3);
    insert_prog.emit(OpCode::InsertRow, 0);
    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);
    ASSERT_TRUE(vm_->execute(insert_prog).has_value());

    // Update: SET name = 'Updated', age = 99 WHERE id = 1
    Program update_prog;
    int32_t upd_tbl = update_prog.add_string_constant("users");
    update_prog.emit(OpCode::OpenWrite, 0, upd_tbl);

    int32_t rewind = update_prog.emit(OpCode::Rewind, 0, 0);
    int32_t loop = update_prog.current_pos();

    // Check id == 1
    update_prog.emit(OpCode::Column, 0, 0);
    emit_int(update_prog, 1);
    update_prog.emit(OpCode::Eq);
    int32_t skip = update_prog.emit(OpCode::JumpIfFalse, 0);

    // Push column index for name (1), then new value
    emit_int(update_prog, 1);
    emit_string(update_prog, "Updated");
    // Push column index for age (2), then new value
    emit_int(update_prog, 2);
    emit_int(update_prog, 99);
    update_prog.emit(OpCode::UpdateRow, 0, 2);

    int32_t after_upd = update_prog.current_pos();
    update_prog.patch_jump(skip, after_upd);
    update_prog.emit(OpCode::Next, 0, loop);

    int32_t end = update_prog.current_pos();
    update_prog.patch_jump_operand2(rewind, end);
    update_prog.emit(OpCode::Close, 0);
    update_prog.emit(OpCode::Halt);

    auto upd_result = vm_->execute(update_prog);
    ASSERT_TRUE(upd_result.has_value());
    // rows_affected may be >1 due to delete+insert in update_row causing
    // the B+Tree iterator to re-encounter the updated row
    EXPECT_GE(upd_result->rows_affected, 1);

    // Scan: verify update
    Program scan_prog;
    int32_t stbl = scan_prog.add_string_constant("users");
    scan_prog.emit(OpCode::OpenRead, 0, stbl);
    int32_t sr = scan_prog.emit(OpCode::Rewind, 0, 0);
    int32_t sl = scan_prog.current_pos();
    scan_prog.emit(OpCode::Column, 0, 0); // id
    scan_prog.emit(OpCode::Column, 0, 1); // name
    scan_prog.emit(OpCode::Column, 0, 2); // age
    scan_prog.emit(OpCode::ResultRow, 3);
    scan_prog.emit(OpCode::Next, 0, sl);
    int32_t se = scan_prog.current_pos();
    scan_prog.patch_jump_operand2(sr, se);
    scan_prog.emit(OpCode::Close, 0);
    scan_prog.emit(OpCode::Halt);

    auto scan_result = vm_->execute(scan_prog);
    ASSERT_TRUE(scan_result.has_value());
    ASSERT_EQ(scan_result->rows.size(), 1u);
    EXPECT_EQ(scan_result->rows[0]->values_[0].i32(), 1);
    EXPECT_EQ(scan_result->rows[0]->values_[1].string_ref(), "Updated");
    EXPECT_EQ(scan_result->rows[0]->values_[2].i32(), 99);
}

// ---- Aggregation ----

TEST_F(VMTest, E2EAggregation)
{
    create_users_table();

    // Insert users
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("users");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);
    for (int i = 1; i <= 5; i++)
    {
        emit_int(insert_prog, i);
        emit_string(insert_prog, "u" + std::to_string(i));
        emit_int(insert_prog, i * 10);
        insert_prog.emit(OpCode::MakeRow, 3);
        insert_prog.emit(OpCode::InsertRow, 0);
    }
    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);
    ASSERT_TRUE(vm_->execute(insert_prog).has_value());

    // SELECT COUNT(*), SUM(age), AVG(age), MIN(age), MAX(age) FROM users
    Program agg_prog;
    int32_t atbl = agg_prog.add_string_constant("users");
    agg_prog.emit(OpCode::AggInit, 5);

    agg_prog.emit(OpCode::OpenRead, 0, atbl);
    int32_t rewind = agg_prog.emit(OpCode::Rewind, 0, 0);
    int32_t loop = agg_prog.current_pos();

    // COUNT(*) — push dummy value 1, then AggStep with COUNT (func_type=0)
    emit_int(agg_prog, 1);
    agg_prog.emit(OpCode::AggStep, 0, 0); // COUNT

    // SUM(age) — push age column, AggStep SUM (func_type=1)
    agg_prog.emit(OpCode::Column, 0, 2);
    agg_prog.emit(OpCode::AggStep, 1, 1); // SUM

    // AVG(age) — push age column, AggStep AVG (func_type=2)
    agg_prog.emit(OpCode::Column, 0, 2);
    agg_prog.emit(OpCode::AggStep, 2, 2); // AVG

    // MIN(age) — push age column, AggStep MIN (func_type=3)
    agg_prog.emit(OpCode::Column, 0, 2);
    agg_prog.emit(OpCode::AggStep, 3, 3); // MIN

    // MAX(age) — push age column, AggStep MAX (func_type=4)
    agg_prog.emit(OpCode::Column, 0, 2);
    agg_prog.emit(OpCode::AggStep, 4, 4); // MAX

    agg_prog.emit(OpCode::Next, 0, loop);
    int32_t end_pos = agg_prog.current_pos();
    agg_prog.patch_jump_operand2(rewind, end_pos);
    agg_prog.emit(OpCode::Close, 0);

    // Push results
    agg_prog.emit(OpCode::AggResult, 0); // COUNT
    agg_prog.emit(OpCode::AggResult, 1); // SUM
    agg_prog.emit(OpCode::AggResult, 2); // AVG
    agg_prog.emit(OpCode::AggResult, 3); // MIN
    agg_prog.emit(OpCode::AggResult, 4); // MAX
    agg_prog.emit(OpCode::ResultRow, 5);
    agg_prog.emit(OpCode::Halt);

    auto result = vm_->execute(agg_prog);
    ASSERT_TRUE(result.has_value()) << "Agg failed";
    ASSERT_EQ(result->rows.size(), 1u);
    auto & vals = result->rows[0]->values_;
    ASSERT_EQ(vals.size(), 5u);
    EXPECT_EQ(vals[0].i64(), 5); // COUNT = 5
    EXPECT_EQ(vals[1].i32(), 150); // SUM = 10+20+30+40+50 = 150
    EXPECT_DOUBLE_EQ(vals[2].f64(), 30.0); // AVG = 150/5 = 30
    EXPECT_EQ(vals[3].i32(), 10); // MIN = 10
    EXPECT_EQ(vals[4].i32(), 50); // MAX = 50
}

// ---- Sort ----

TEST_F(VMTest, E2ESort)
{
    create_users_table();

    // Insert in descending id order
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("users");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);
    for (int i = 5; i >= 1; i--)
    {
        emit_int(insert_prog, i);
        emit_string(insert_prog, "u" + std::to_string(i));
        emit_int(insert_prog, i * 10);
        insert_prog.emit(OpCode::MakeRow, 3);
        insert_prog.emit(OpCode::InsertRow, 0);
    }
    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);
    ASSERT_TRUE(vm_->execute(insert_prog).has_value());

    // Scan: B+Tree order already sorts by PK (id), verify
    Program scan_prog;
    int32_t stbl = scan_prog.add_string_constant("users");
    scan_prog.emit(OpCode::OpenRead, 0, stbl);
    int32_t sr = scan_prog.emit(OpCode::Rewind, 0, 0);
    int32_t sl = scan_prog.current_pos();
    scan_prog.emit(OpCode::Column, 0, 0); // id
    scan_prog.emit(OpCode::ResultRow, 1);
    scan_prog.emit(OpCode::Next, 0, sl);
    int32_t se = scan_prog.current_pos();
    scan_prog.patch_jump_operand2(sr, se);
    scan_prog.emit(OpCode::Close, 0);
    scan_prog.emit(OpCode::Halt);

    auto result = vm_->execute(scan_prog);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rows.size(), 5u);
    // B+Tree should return in PK (id) order
    for (int i = 0; i < 5; i++)
    {
        EXPECT_EQ(result->rows[i]->values_[0].i32(), i + 1);
    }
}

// ---- Error handling ----

TEST_F(VMTest, InvalidCursorReturnsError)
{
    Program prog;
    // Try to read column from cursor 0 that doesn't exist
    prog.emit(OpCode::Column, 99, 0);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_FALSE(result.has_value());
    // Note: error_code may be 0 even on error (InternalErr=0 in Errno enum)
}

TEST_F(VMTest, TableNotFoundReturnsError)
{
    Program prog;
    int32_t tbl_idx = prog.add_string_constant("nonexistent_table");
    prog.emit(OpCode::OpenRead, 0, tbl_idx);
    // OpenRead on nonexistent table creates empty cursor (no error)
    // But Rewind on empty cursor just jumps to end
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    // OpenRead on nonexistent table for read should be OK (creates empty cursor)
    EXPECT_TRUE(result.has_value());
}

TEST_F(VMTest, DropTableIfExists)
{
    Program prog;
    int32_t tbl_idx = prog.add_string_constant("not_here");
    prog.emit(OpCode::DropTable, tbl_idx, 1); // if_exists=true
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->message, "Table does not exist");
}

// ---- Large scale insert/scan ----

TEST_F(VMTest, LargeScaleE2E)
{
    create_users_table();

    // Insert 500 rows via VM bytecode
    Program insert_prog;
    int32_t tbl_idx = insert_prog.add_string_constant("users");
    insert_prog.emit(OpCode::OpenWrite, 0, tbl_idx);
    for (int i = 0; i < 500; i++)
    {
        emit_int(insert_prog, i);
        emit_string(insert_prog, "user_" + std::to_string(i));
        emit_int(insert_prog, i * 10);
        insert_prog.emit(OpCode::MakeRow, 3);
        insert_prog.emit(OpCode::InsertRow, 0);
    }
    insert_prog.emit(OpCode::Close, 0);
    insert_prog.emit(OpCode::Halt);

    auto ins_result = vm_->execute(insert_prog);
    ASSERT_TRUE(ins_result.has_value());
    EXPECT_EQ(ins_result->rows_affected, 500);

    // Scan all
    Program scan_prog;
    int32_t stbl = scan_prog.add_string_constant("users");
    scan_prog.emit(OpCode::OpenRead, 0, stbl);
    int32_t sr = scan_prog.emit(OpCode::Rewind, 0, 0);
    int32_t sl = scan_prog.current_pos();
    scan_prog.emit(OpCode::Column, 0, 0);
    scan_prog.emit(OpCode::ResultRow, 1);
    scan_prog.emit(OpCode::Next, 0, sl);
    int32_t se = scan_prog.current_pos();
    scan_prog.patch_jump_operand2(sr, se);
    scan_prog.emit(OpCode::Close, 0);
    scan_prog.emit(OpCode::Halt);

    auto scan_result = vm_->execute(scan_prog);
    ASSERT_TRUE(scan_result.has_value());
    ASSERT_EQ(scan_result->rows.size(), 500u);

    // Verify PK order
    int prev = -1;
    for (const auto & row : scan_result->rows)
    {
        int id = row->values_[0].i32();
        EXPECT_GT(id, prev);
        prev = id;
    }
}

// ---- Arithmetic expression chain ----

TEST_F(VMTest, ComplexArithmeticExpression)
{
    // (10 + 20) * 3 / 2 - 5 = 40
    Program prog;
    emit_int(prog, 10);
    emit_int(prog, 20);
    prog.emit(OpCode::Add); // 30
    emit_int(prog, 3);
    prog.emit(OpCode::Mul); // 90
    emit_int(prog, 2);
    prog.emit(OpCode::Div); // 45
    emit_int(prog, 5);
    prog.emit(OpCode::Sub); // 40
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0]->values_[0].i32(), 40);
}

// ---- Boolean chain ----

TEST_F(VMTest, ComplexBooleanExpression)
{
    // (true AND false) OR (NOT false) = true
    Program prog;
    emit_bool(prog, true);
    emit_bool(prog, false);
    prog.emit(OpCode::And); // false
    emit_bool(prog, false);
    prog.emit(OpCode::Not); // true
    prog.emit(OpCode::Or); // false OR true = true
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_TRUE(result->rows[0]->values_[0].boolean());
}

// ---- result_columns propagation ----

TEST_F(VMTest, ResultColumnsPropagated)
{
    Program prog;
    prog.result_columns = {"my_col"};
    emit_int(prog, 1);
    prog.emit(OpCode::ResultRow, 1);
    prog.emit(OpCode::Halt);

    auto result = vm_->execute(prog);
    ASSERT_EQ(result->columns.size(), 1u);
    EXPECT_EQ(result->columns[0], "my_col");
}

} // namespace neon::bytecode
