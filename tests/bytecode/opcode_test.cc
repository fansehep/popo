/**
 * OpCode and Instruction unit tests.
 * Tests: opcode_name(), Instruction constructors, opcode value uniqueness.
 */
#include <set>
#include <gtest/gtest.h>
#include "src/bytecode/opcode.h"

namespace neon::bytecode
{

// ---- opcode_name ----

TEST(OpCodeTest, AllOpcodesHaveNames)
{
    // Every opcode should map to a non-"Unknown" name
    std::vector<OpCode> all = {
        OpCode::Const,       OpCode::Pop,          OpCode::Column,     OpCode::Add,          OpCode::Sub,        OpCode::Mul,
        OpCode::Div,         OpCode::Neg,          OpCode::Eq,         OpCode::Ne,           OpCode::Lt,         OpCode::Le,
        OpCode::Gt,          OpCode::Ge,           OpCode::And,        OpCode::Or,           OpCode::Not,        OpCode::Jump,
        OpCode::JumpIfFalse, OpCode::JumpIfTrue,   OpCode::Halt,       OpCode::OpenRead,     OpCode::OpenWrite,  OpCode::Rewind,
        OpCode::Next,        OpCode::Close,        OpCode::ResultRow,  OpCode::ColumnName,   OpCode::MakeRow,    OpCode::InsertRow,
        OpCode::DeleteRow,   OpCode::UpdateRow,    OpCode::AggInit,    OpCode::AggStep,      OpCode::AggResult,  OpCode::AggReset,
        OpCode::SorterOpen,  OpCode::SorterInsert, OpCode::SorterSort, OpCode::SorterRewind, OpCode::SorterNext, OpCode::SorterColumn,
        OpCode::SortKeyDef,  OpCode::CreateTable,  OpCode::DropTable,  OpCode::Noop,         OpCode::Dup,
    };
    for (auto op : all)
    {
        EXPECT_NE(opcode_name(op), "Unknown") << "OpCode missing name";
    }
}

TEST(OpCodeTest, OpcodeNamesCorrect)
{
    EXPECT_EQ(opcode_name(OpCode::Const), "Const");
    EXPECT_EQ(opcode_name(OpCode::Pop), "Pop");
    EXPECT_EQ(opcode_name(OpCode::Add), "Add");
    EXPECT_EQ(opcode_name(OpCode::Sub), "Sub");
    EXPECT_EQ(opcode_name(OpCode::Mul), "Mul");
    EXPECT_EQ(opcode_name(OpCode::Div), "Div");
    EXPECT_EQ(opcode_name(OpCode::Neg), "Neg");
    EXPECT_EQ(opcode_name(OpCode::Eq), "Eq");
    EXPECT_EQ(opcode_name(OpCode::Ne), "Ne");
    EXPECT_EQ(opcode_name(OpCode::Lt), "Lt");
    EXPECT_EQ(opcode_name(OpCode::Le), "Le");
    EXPECT_EQ(opcode_name(OpCode::Gt), "Gt");
    EXPECT_EQ(opcode_name(OpCode::Ge), "Ge");
    EXPECT_EQ(opcode_name(OpCode::And), "And");
    EXPECT_EQ(opcode_name(OpCode::Or), "Or");
    EXPECT_EQ(opcode_name(OpCode::Not), "Not");
    EXPECT_EQ(opcode_name(OpCode::Jump), "Jump");
    EXPECT_EQ(opcode_name(OpCode::JumpIfFalse), "JumpIfFalse");
    EXPECT_EQ(opcode_name(OpCode::JumpIfTrue), "JumpIfTrue");
    EXPECT_EQ(opcode_name(OpCode::Halt), "Halt");
    EXPECT_EQ(opcode_name(OpCode::OpenRead), "OpenRead");
    EXPECT_EQ(opcode_name(OpCode::OpenWrite), "OpenWrite");
    EXPECT_EQ(opcode_name(OpCode::Rewind), "Rewind");
    EXPECT_EQ(opcode_name(OpCode::Next), "Next");
    EXPECT_EQ(opcode_name(OpCode::Close), "Close");
    EXPECT_EQ(opcode_name(OpCode::ResultRow), "ResultRow");
    EXPECT_EQ(opcode_name(OpCode::Noop), "Noop");
    EXPECT_EQ(opcode_name(OpCode::Dup), "Dup");
}

TEST(OpCodeTest, OpcodeValuesDistinct)
{
    std::set<uint8_t> seen;
    std::vector<OpCode> all = {
        OpCode::Const,       OpCode::Pop,          OpCode::Column,     OpCode::Add,          OpCode::Sub,        OpCode::Mul,
        OpCode::Div,         OpCode::Neg,          OpCode::Eq,         OpCode::Ne,           OpCode::Lt,         OpCode::Le,
        OpCode::Gt,          OpCode::Ge,           OpCode::And,        OpCode::Or,           OpCode::Not,        OpCode::Jump,
        OpCode::JumpIfFalse, OpCode::JumpIfTrue,   OpCode::Halt,       OpCode::OpenRead,     OpCode::OpenWrite,  OpCode::Rewind,
        OpCode::Next,        OpCode::Close,        OpCode::ResultRow,  OpCode::ColumnName,   OpCode::MakeRow,    OpCode::InsertRow,
        OpCode::DeleteRow,   OpCode::UpdateRow,    OpCode::AggInit,    OpCode::AggStep,      OpCode::AggResult,  OpCode::AggReset,
        OpCode::SorterOpen,  OpCode::SorterInsert, OpCode::SorterSort, OpCode::SorterRewind, OpCode::SorterNext, OpCode::SorterColumn,
        OpCode::SortKeyDef,  OpCode::CreateTable,  OpCode::DropTable,  OpCode::Noop,         OpCode::Dup,
    };
    for (auto op : all)
    {
        EXPECT_TRUE(seen.insert(static_cast<uint8_t>(op)).second) << "Duplicate opcode value: " << opcode_name(op);
    }
}

// ---- Instruction constructors ----

TEST(InstructionTest, DefaultConstructor)
{
    Instruction inst;
    EXPECT_EQ(inst.op, OpCode::Noop);
    EXPECT_EQ(inst.operand1, 0);
    EXPECT_EQ(inst.operand2, 0);
    EXPECT_EQ(inst.operand3, 0);
}

TEST(InstructionTest, SingleArgConstructor)
{
    Instruction inst(OpCode::Halt);
    EXPECT_EQ(inst.op, OpCode::Halt);
    EXPECT_EQ(inst.operand1, 0);
    EXPECT_EQ(inst.operand2, 0);
    EXPECT_EQ(inst.operand3, 0);
}

TEST(InstructionTest, TwoArgConstructor)
{
    Instruction inst(OpCode::Jump, 42);
    EXPECT_EQ(inst.op, OpCode::Jump);
    EXPECT_EQ(inst.operand1, 42);
    EXPECT_EQ(inst.operand2, 0);
    EXPECT_EQ(inst.operand3, 0);
}

TEST(InstructionTest, ThreeArgConstructor)
{
    Instruction inst(OpCode::Column, 0, 2);
    EXPECT_EQ(inst.op, OpCode::Column);
    EXPECT_EQ(inst.operand1, 0);
    EXPECT_EQ(inst.operand2, 2);
    EXPECT_EQ(inst.operand3, 0);
}

TEST(InstructionTest, FourArgConstructor)
{
    Instruction inst(OpCode::SortKeyDef, 0, 1, 3);
    EXPECT_EQ(inst.op, OpCode::SortKeyDef);
    EXPECT_EQ(inst.operand1, 0);
    EXPECT_EQ(inst.operand2, 1);
    EXPECT_EQ(inst.operand3, 3);
}

} // namespace neon::bytecode
