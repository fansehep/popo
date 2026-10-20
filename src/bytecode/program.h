#pragma once
#include <string>
#include <variant>
#include <vector>
#include "bytecode/opcode.h"
#include "parser/ast.h"
#include "type/value.h"

namespace popo::bytecode
{

using ConstantValue = std::variant<popo::type::Value, std::string, std::vector<popo::parser::ColumnDef>>;

struct Program
{
    std::vector<Instruction> instructions;
    std::vector<ConstantValue> constants;
    std::vector<std::string> result_columns;

    int32_t add_constant(const popo::type::Value & val)
    {
        constants.emplace_back(val);
        return static_cast<int32_t>(constants.size() - 1);
    }

    int32_t add_string_constant(const std::string & str)
    {
        constants.emplace_back(str);
        return static_cast<int32_t>(constants.size() - 1);
    }

    int32_t add_column_defs(const std::vector<popo::parser::ColumnDef> & defs)
    {
        constants.emplace_back(defs);
        return static_cast<int32_t>(constants.size() - 1);
    }

    int32_t emit(Instruction inst)
    {
        instructions.push_back(inst);
        return static_cast<int32_t>(instructions.size() - 1);
    }

    int32_t emit(OpCode op) { return emit(Instruction(op)); }
    int32_t emit(OpCode op, int32_t o1) { return emit(Instruction(op, o1)); }
    int32_t emit(OpCode op, int32_t o1, int32_t o2) { return emit(Instruction(op, o1, o2)); }
    int32_t emit(OpCode op, int32_t o1, int32_t o2, int32_t o3) { return emit(Instruction(op, o1, o2, o3)); }

    void patch_jump(int32_t inst_index, int32_t target) { instructions[inst_index].operand1 = target; }
    void patch_jump_operand2(int32_t inst_index, int32_t target) { instructions[inst_index].operand2 = target; }

    int32_t current_pos() const { return static_cast<int32_t>(instructions.size()); }

    std::string disassemble() const;
};

} // namespace popo::bytecode
