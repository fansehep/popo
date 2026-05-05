#pragma once
#include <cstdint>
#include <string>

namespace neon::bytecode
{

enum class OpCode : uint8_t
{
    Const,
    Pop,
    Column,
    Add,
    Sub,
    Mul,
    Div,
    Neg,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Not,
    Jump,
    JumpIfFalse,
    JumpIfTrue,
    Halt,
    OpenRead,
    OpenWrite,
    Rewind,
    Next,
    Close,
    ResultRow,
    ColumnName,
    MakeRow,
    InsertRow,
    DeleteRow,
    UpdateRow,
    AggInit,
    AggStep,
    AggResult,
    AggReset,
    SorterOpen,
    SorterInsert,
    SorterSort,
    SorterRewind,
    SorterNext,
    SorterColumn,
    SortKeyDef,
    CreateTable,
    DropTable,
    ShowTables,
    Describe,
    ShowDatabases,
    ShowVariables,
    SetVar,
    SelectDatabase,
    SelectSystemVar,
    Noop,
    Dup,
    SeekByPk,
};

struct Instruction
{
    OpCode op;
    int32_t operand1 = 0;
    int32_t operand2 = 0;
    int32_t operand3 = 0;

    Instruction() : op(OpCode::Noop) { }
    Instruction(OpCode op) : op(op) { }
    Instruction(OpCode op, int32_t o1) : op(op), operand1(o1) { }
    Instruction(OpCode op, int32_t o1, int32_t o2) : op(op), operand1(o1), operand2(o2) { }
    Instruction(OpCode op, int32_t o1, int32_t o2, int32_t o3) : op(op), operand1(o1), operand2(o2), operand3(o3) { }
};

inline std::string opcode_name(OpCode op)
{
    switch (op)
    {
        case OpCode::Const:
            return "Const";
        case OpCode::Pop:
            return "Pop";
        case OpCode::Column:
            return "Column";
        case OpCode::Add:
            return "Add";
        case OpCode::Sub:
            return "Sub";
        case OpCode::Mul:
            return "Mul";
        case OpCode::Div:
            return "Div";
        case OpCode::Neg:
            return "Neg";
        case OpCode::Eq:
            return "Eq";
        case OpCode::Ne:
            return "Ne";
        case OpCode::Lt:
            return "Lt";
        case OpCode::Le:
            return "Le";
        case OpCode::Gt:
            return "Gt";
        case OpCode::Ge:
            return "Ge";
        case OpCode::And:
            return "And";
        case OpCode::Or:
            return "Or";
        case OpCode::Not:
            return "Not";
        case OpCode::Jump:
            return "Jump";
        case OpCode::JumpIfFalse:
            return "JumpIfFalse";
        case OpCode::JumpIfTrue:
            return "JumpIfTrue";
        case OpCode::Halt:
            return "Halt";
        case OpCode::OpenRead:
            return "OpenRead";
        case OpCode::OpenWrite:
            return "OpenWrite";
        case OpCode::Rewind:
            return "Rewind";
        case OpCode::Next:
            return "Next";
        case OpCode::Close:
            return "Close";
        case OpCode::ResultRow:
            return "ResultRow";
        case OpCode::ColumnName:
            return "ColumnName";
        case OpCode::MakeRow:
            return "MakeRow";
        case OpCode::InsertRow:
            return "InsertRow";
        case OpCode::DeleteRow:
            return "DeleteRow";
        case OpCode::UpdateRow:
            return "UpdateRow";
        case OpCode::AggInit:
            return "AggInit";
        case OpCode::AggStep:
            return "AggStep";
        case OpCode::AggResult:
            return "AggResult";
        case OpCode::AggReset:
            return "AggReset";
        case OpCode::SorterOpen:
            return "SorterOpen";
        case OpCode::SorterInsert:
            return "SorterInsert";
        case OpCode::SorterSort:
            return "SorterSort";
        case OpCode::SorterRewind:
            return "SorterRewind";
        case OpCode::SorterNext:
            return "SorterNext";
        case OpCode::SorterColumn:
            return "SorterColumn";
        case OpCode::SortKeyDef:
            return "SortKeyDef";
        case OpCode::CreateTable:
            return "CreateTable";
        case OpCode::DropTable:
            return "DropTable";
        case OpCode::ShowTables:
            return "ShowTables";
        case OpCode::Describe:
            return "Describe";
        case OpCode::ShowDatabases:
            return "ShowDatabases";
        case OpCode::ShowVariables:
            return "ShowVariables";
        case OpCode::SetVar:
            return "SetVar";
        case OpCode::SelectDatabase:
            return "SelectDatabase";
        case OpCode::SelectSystemVar:
            return "SelectSystemVar";
        case OpCode::Noop:
            return "Noop";
        case OpCode::Dup:
            return "Dup";
        case OpCode::SeekByPk:
            return "SeekByPk";
        default:
            return "Unknown";
    }
}

} // namespace neon::bytecode
