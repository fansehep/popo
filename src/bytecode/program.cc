#include "bytecode/program.h"
#include <sstream>

namespace neon::bytecode
{

std::string Program::disassemble() const
{
    std::ostringstream oss;
    for (size_t i = 0; i < instructions.size(); ++i)
    {
        const auto & inst = instructions[i];
        oss << i << ": " << opcode_name(inst.op);
        switch (inst.op)
        {
            case OpCode::Const:
            case OpCode::Pop:
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div:
            case OpCode::Neg:
            case OpCode::Eq:
            case OpCode::Ne:
            case OpCode::Lt:
            case OpCode::Le:
            case OpCode::Gt:
            case OpCode::Ge:
            case OpCode::And:
            case OpCode::Or:
            case OpCode::Not:
            case OpCode::Halt:
            case OpCode::Noop:
            case OpCode::Dup:
            case OpCode::AggReset:
            case OpCode::SorterSort:
                break;
            case OpCode::Column:
            case OpCode::OpenRead:
            case OpCode::OpenWrite:
            case OpCode::SortKeyDef:
                oss << " " << inst.operand1 << " " << inst.operand2;
                if (inst.operand3 != 0)
                    oss << " " << inst.operand3;
                break;
            case OpCode::Rewind:
            case OpCode::Next:
            case OpCode::SorterRewind:
            case OpCode::SorterNext:
                oss << " " << inst.operand1 << " ->" << inst.operand2;
                break;
            case OpCode::Jump:
            case OpCode::JumpIfFalse:
            case OpCode::JumpIfTrue:
                oss << " ->" << inst.operand1;
                break;
            case OpCode::Close:
            case OpCode::TableCount:
            case OpCode::ResultRow:
            case OpCode::MakeRow:
            case OpCode::InsertRow:
            case OpCode::DeleteRow:
            case OpCode::AggInit:
            case OpCode::AggStep:
            case OpCode::AggResult:
            case OpCode::SorterOpen:
            case OpCode::SorterInsert:
            case OpCode::SorterColumn:
            case OpCode::CreateTable:
            case OpCode::DropTable:
                oss << " " << inst.operand1;
                if (inst.operand2 != 0)
                    oss << " " << inst.operand2;
                break;
            case OpCode::ColumnName:
            case OpCode::UpdateRow:
                oss << " " << inst.operand1 << " " << inst.operand2;
                break;
        }
        oss << "\n";
    }
    return oss.str();
}

} // namespace neon::bytecode
