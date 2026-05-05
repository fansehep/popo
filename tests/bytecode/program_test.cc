/**
 * Program unit tests.
 * Tests: emit, add_constant, patch_jump, current_pos, disassemble.
 */
#include <gtest/gtest.h>
#include "src/bytecode/program.h"

namespace neon::bytecode
{

class ProgramTest : public ::testing::Test
{
protected:
    Program program;
};

// ---- emit ----

TEST_F(ProgramTest, EmitOpOnly)
{
    EXPECT_EQ(program.current_pos(), 0);
    int32_t idx = program.emit(OpCode::Halt);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(program.current_pos(), 1);
    ASSERT_EQ(program.instructions.size(), 1u);
    EXPECT_EQ(program.instructions[0].op, OpCode::Halt);
}

TEST_F(ProgramTest, EmitWithOperands)
{
    program.emit(OpCode::Const, 0);
    program.emit(OpCode::Column, 1, 2);
    program.emit(OpCode::SortKeyDef, 0, 1, 3);
    EXPECT_EQ(program.instructions.size(), 3u);
    EXPECT_EQ(program.instructions[2].operand3, 3);
}

TEST_F(ProgramTest, EmitReturnsCorrectIndices)
{
    int32_t a = program.emit(OpCode::Noop);
    int32_t b = program.emit(OpCode::Noop);
    int32_t c = program.emit(OpCode::Noop);
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(c, 2);
}

TEST_F(ProgramTest, EmitMultipleInstructions)
{
    for (int i = 0; i < 100; ++i)
    {
        program.emit(OpCode::Noop);
    }
    EXPECT_EQ(program.instructions.size(), 100u);
    EXPECT_EQ(program.current_pos(), 100);
}

// ---- add_constant (Value) ----

TEST_F(ProgramTest, AddValueConstant)
{
    int32_t idx = program.add_constant(neon::type::Value(neon::type::ValueType::Int32, 42));
    EXPECT_EQ(idx, 0);
    ASSERT_EQ(program.constants.size(), 1u);
    const auto & c = program.constants[0];
    ASSERT_TRUE(std::holds_alternative<neon::type::Value>(c));
    EXPECT_EQ(std::get<neon::type::Value>(c).i32(), 42);
}

TEST_F(ProgramTest, AddMultipleValueConstants)
{
    program.add_constant(neon::type::Value(neon::type::ValueType::Int8, 1));
    program.add_constant(neon::type::Value(neon::type::ValueType::Int16, 2));
    program.add_constant(neon::type::Value(neon::type::ValueType::Int64, 3));
    program.add_constant(neon::type::Value(neon::type::ValueType::Float64, 3.14));
    program.add_constant(neon::type::Value(neon::type::ValueType::Bool, true));
    program.add_constant(neon::type::Value(neon::type::ValueType::String, std::string("hello")));
    EXPECT_EQ(program.constants.size(), 6u);
    EXPECT_EQ(std::get<neon::type::Value>(program.constants[3]).f64(), 3.14);
    EXPECT_TRUE(std::get<neon::type::Value>(program.constants[4]).boolean());
}

// ---- add_string_constant ----

TEST_F(ProgramTest, AddStringConstant)
{
    int32_t idx = program.add_string_constant("test_table");
    EXPECT_EQ(idx, 0);
    ASSERT_EQ(program.constants.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<std::string>(program.constants[0]));
    EXPECT_EQ(std::get<std::string>(program.constants[0]), "test_table");
}

TEST_F(ProgramTest, StringAndValueConstantsInterleave)
{
    program.add_constant(neon::type::Value(neon::type::ValueType::Int32, 10));
    program.add_string_constant("users");
    program.add_constant(neon::type::Value(neon::type::ValueType::Int32, 20));
    program.add_string_constant("orders");
    EXPECT_EQ(program.constants.size(), 4u);
    EXPECT_EQ(std::get<neon::type::Value>(program.constants[0]).i32(), 10);
    EXPECT_EQ(std::get<std::string>(program.constants[1]), "users");
    EXPECT_EQ(std::get<neon::type::Value>(program.constants[2]).i32(), 20);
    EXPECT_EQ(std::get<std::string>(program.constants[3]), "orders");
}

// ---- add_column_defs ----

TEST_F(ProgramTest, AddColumnDefs)
{
    std::vector<neon::parser::ColumnDef> defs;
    neon::parser::ColumnDef col;
    col.name = "id";
    col.data_type = neon::type::ValueType::Int32;
    col.is_primary_key = true;
    defs.push_back(col);

    col.name = "name";
    col.data_type = neon::type::ValueType::String;
    col.is_primary_key = false;
    col.is_nullable = true;
    defs.push_back(col);

    int32_t idx = program.add_column_defs(defs);
    EXPECT_EQ(idx, 0);
    ASSERT_EQ(program.constants.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<std::vector<neon::parser::ColumnDef>>(program.constants[0]));
    const auto & stored = std::get<std::vector<neon::parser::ColumnDef>>(program.constants[0]);
    EXPECT_EQ(stored.size(), 2u);
    EXPECT_EQ(stored[0].name, "id");
    EXPECT_TRUE(stored[0].is_primary_key);
}

// ---- patch_jump ----

TEST_F(ProgramTest, PatchJump)
{
    int32_t jump_inst = program.emit(OpCode::Jump, -1); // placeholder
    program.emit(OpCode::Noop);
    program.emit(OpCode::Noop);
    int32_t target = program.current_pos(); // = 3

    program.patch_jump(jump_inst, target);
    EXPECT_EQ(program.instructions[jump_inst].operand1, target);
}

TEST_F(ProgramTest, PatchJumpOperand2)
{
    int32_t jump_inst = program.emit(OpCode::Rewind, 0, -1); // placeholder operand2
    program.emit(OpCode::Noop);
    int32_t target = program.current_pos();

    program.patch_jump_operand2(jump_inst, target);
    EXPECT_EQ(program.instructions[jump_inst].operand2, target);
}

// ---- current_pos ----

TEST_F(ProgramTest, CurrentPosTracksInstructions)
{
    EXPECT_EQ(program.current_pos(), 0);
    program.emit(OpCode::Noop);
    EXPECT_EQ(program.current_pos(), 1);
    program.emit(OpCode::Noop);
    program.emit(OpCode::Noop);
    EXPECT_EQ(program.current_pos(), 3);
}

// ---- disassemble ----

TEST_F(ProgramTest, DisassembleEmpty)
{
    std::string s = program.disassemble();
    EXPECT_TRUE(s.empty());
}

TEST_F(ProgramTest, DisassembleNonEmpty)
{
    program.emit(OpCode::Halt);
    std::string s = program.disassemble();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("Halt"), std::string::npos);
}

TEST_F(ProgramTest, DisassembleIncludesOpcodeNames)
{
    program.emit(OpCode::Const, 0);
    program.emit(OpCode::Add);
    program.emit(OpCode::Jump, 5);
    program.emit(OpCode::Halt);

    std::string s = program.disassemble();
    EXPECT_NE(s.find("Const"), std::string::npos);
    EXPECT_NE(s.find("Add"), std::string::npos);
    EXPECT_NE(s.find("Jump"), std::string::npos);
    EXPECT_NE(s.find("Halt"), std::string::npos);
}

TEST_F(ProgramTest, DisassembleIncludesLineNumbers)
{
    program.emit(OpCode::Noop);
    program.emit(OpCode::Noop);
    program.emit(OpCode::Noop);

    std::string s = program.disassemble();
    EXPECT_NE(s.find("0:"), std::string::npos);
    EXPECT_NE(s.find("1:"), std::string::npos);
    EXPECT_NE(s.find("2:"), std::string::npos);
}

// ---- Program construction from scratch ----

TEST_F(ProgramTest, BuildSimpleProgram)
{
    // Build: Halt
    program.emit(OpCode::Halt);
    EXPECT_EQ(program.instructions.size(), 1u);
    EXPECT_EQ(program.constants.size(), 0u);
}

TEST_F(ProgramTest, BuildProgramWithConstants)
{
    int32_t val_idx = program.add_constant(neon::type::Value(neon::type::ValueType::Int32, 10));
    int32_t str_idx = program.add_string_constant("hi");

    program.emit(OpCode::Const, val_idx);
    program.emit(OpCode::Halt);

    EXPECT_EQ(program.constants.size(), 2u);
    EXPECT_EQ(program.instructions.size(), 2u);
}

// ---- Jump chaining pattern ----

TEST_F(ProgramTest, JumpForwardPattern)
{
    // Simulate a conditional branch: JumpIfFalse past a block, then Halt
    int32_t cond_jump = program.emit(OpCode::JumpIfFalse, -1); // placeholder
    // "then" block:
    program.emit(OpCode::Noop);
    program.emit(OpCode::Noop);
    // after block:
    int32_t after_pos = program.current_pos();
    program.patch_jump(cond_jump, after_pos);
    program.emit(OpCode::Halt);

    EXPECT_EQ(program.instructions[0].operand1, after_pos);
    EXPECT_EQ(program.instructions[0].op, OpCode::JumpIfFalse);
}

// ---- result_columns ----

TEST_F(ProgramTest, ResultColumnsInitiallyEmpty)
{
    EXPECT_TRUE(program.result_columns.empty());
}

TEST_F(ProgramTest, ResultColumnsCanBeSet)
{
    program.result_columns.push_back("col1");
    program.result_columns.push_back("col2");
    ASSERT_EQ(program.result_columns.size(), 2u);
    EXPECT_EQ(program.result_columns[0], "col1");
    EXPECT_EQ(program.result_columns[1], "col2");
}

} // namespace neon::bytecode
