#include "bytecode/compiler.h"
#include <format>
#include <stdexcept>

namespace neon::bytecode
{

Compiler::Compiler(neon::catalog::SchemaManager & schema_mgr) : schema_mgr_(schema_mgr)
{
}

neon::base::Result<Program> Compiler::compile(const neon::planner::PlanNodePtr & plan)
{
    program_ = Program();
    next_cursor_id_ = 0;
    next_sorter_id_ = 0;
    table_to_cursor_.clear();
    cursor_schemas_.clear();
    agg_expr_to_index_.clear();
    in_agg_step_context_ = false;

    try
    {
        compile_node(plan);
        return program_;
    }
    catch (const std::exception & e)
    {
        return std::unexpected(Error{neon::base::Unknown, e.what()});
    }
}

void Compiler::compile_node(const neon::planner::PlanNodePtr & node)
{
    if (!node)
        return;
    switch (node->type)
    {
        case neon::planner::PlanNodeType::Scan:
            compile_scan(static_cast<const neon::planner::ScanNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Filter:
            compile_filter(static_cast<const neon::planner::FilterNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Project:
            compile_project(static_cast<const neon::planner::ProjectNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Join:
            compile_join(static_cast<const neon::planner::JoinNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Agg:
            compile_agg(static_cast<const neon::planner::AggNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Sort:
            compile_sort(static_cast<const neon::planner::SortNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Limit:
            compile_limit(static_cast<const neon::planner::LimitNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Insert:
            compile_insert(static_cast<const neon::planner::InsertNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Update:
            compile_update(static_cast<const neon::planner::UpdateNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Delete:
            compile_delete(static_cast<const neon::planner::DeleteNode &>(*node));
            break;
        case neon::planner::PlanNodeType::CreateTable:
            compile_create_table(static_cast<const neon::planner::CreateTableNode &>(*node));
            break;
        case neon::planner::PlanNodeType::CreateIndex:
            compile_create_index(static_cast<const neon::planner::CreateIndexNode &>(*node));
            break;
        case neon::planner::PlanNodeType::DropTable:
            compile_drop_table(static_cast<const neon::planner::DropTableNode &>(*node));
            break;
        case neon::planner::PlanNodeType::ShowTables:
            compile_show_tables(static_cast<const neon::planner::ShowTablesNode &>(*node));
            break;
        case neon::planner::PlanNodeType::Describe:
            compile_describe(static_cast<const neon::planner::DescribeNode &>(*node));
            break;
        case neon::planner::PlanNodeType::ShowDatabases:
            compile_show_databases(static_cast<const neon::planner::ShowDatabasesNode &>(*node));
            break;
        case neon::planner::PlanNodeType::SetVar:
            compile_set_var(static_cast<const neon::planner::SetVarNode &>(*node));
            break;
        case neon::planner::PlanNodeType::ShowVariables:
            compile_show_variables(static_cast<const neon::planner::ShowVariablesNode &>(*node));
            break;
        case neon::planner::PlanNodeType::ShowStatus:
            compile_show_status(static_cast<const neon::planner::ShowStatusNode &>(*node));
            break;
        case neon::planner::PlanNodeType::SelectDatabase:
            compile_select_database(static_cast<const neon::planner::SelectDatabaseNode &>(*node));
            break;
        case neon::planner::PlanNodeType::SelectSystemVar:
            compile_select_system_var(static_cast<const neon::planner::SelectSystemVarNode &>(*node));
            break;
    }
}

void Compiler::compile_scan(const neon::planner::ScanNode & node)
{
    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::OpenRead, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(node.table_name);
    if (qr && *qr)
    {
        cursor_schemas_[cursor] = *qr;
        table_to_cursor_[node.table_name] = cursor;
        if (!node.alias.empty())
        {
            table_to_cursor_[node.alias] = cursor;
        }
    }

    int32_t rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    int32_t loop_start = program_.current_pos();

    emit_all_columns(cursor);
    program_.emit(OpCode::ResultRow, static_cast<int32_t>(node.column_names.size()));

    int32_t next_pos = program_.current_pos();
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_pos);
    program_.emit(OpCode::Close, cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_filter(const neon::planner::FilterNode & node)
{
    compile_node(node.child);
}

void Compiler::compile_project(const neon::planner::ProjectNode & node)
{
    compile_select(node);
}

void Compiler::compile_join(const neon::planner::JoinNode & node)
{
    SelectPipeline pipe;
    pipe.join = &node;
    pipe.project = nullptr;
    compile_select_pipeline(pipe);
}

void Compiler::compile_agg(const neon::planner::AggNode & node)
{
    compile_node(node.child);
}

void Compiler::compile_sort(const neon::planner::SortNode & node)
{
    compile_node(node.child);
}

void Compiler::compile_limit(const neon::planner::LimitNode & node)
{
    compile_node(node.child);
}

void Compiler::compile_insert(const neon::planner::InsertNode & node)
{
    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::OpenWrite, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(node.table_name);
    if (qr && *qr)
    {
        cursor_schemas_[cursor] = *qr;
    }

    for (const auto & row_values : node.values)
    {
        for (const auto & expr : row_values)
        {
            compile_expr(expr);
        }
        program_.emit(OpCode::MakeRow, static_cast<int32_t>(row_values.size()));
        program_.emit(OpCode::InsertRow, cursor);
    }

    program_.emit(OpCode::Close, cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_update(const neon::planner::UpdateNode & node)
{
    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::OpenWrite, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(node.table_name);
    if (qr && *qr)
    {
        cursor_schemas_[cursor] = *qr;
        table_to_cursor_[node.table_name] = cursor;
    }

    // Try PK-equality fast path: WHERE pk_col = const
    auto pk_eq = detect_pk_equality(node.where_clause, cursor);
    if (pk_eq.has_value())
    {
        auto [_, pk_col_idx, pk_value] = pk_eq.value();
        // Push PK literal value onto stack for SeekByPk
        program_.emit(OpCode::Const, program_.add_constant(pk_value));
        // SeekByPk(cursor, skip_target) — jump to Close/Halt if not found
        int32_t seek_inst = program_.emit(OpCode::SeekByPk, cursor, 0);

        // Emit assignments (update column values)
        for (const auto & assign : node.assignments)
        {
            int32_t col_idx = find_column_index(cursor, assign.column_name);
            if (col_idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found", assign.column_name));
            }
            program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, col_idx)));
            compile_expr(assign.value);
        }
        program_.emit(OpCode::UpdateRow, cursor, static_cast<int32_t>(node.assignments.size()));

        int32_t end_pos = program_.current_pos();
        program_.patch_jump_operand2(seek_inst, end_pos);
        program_.emit(OpCode::Close, cursor);
        program_.emit(OpCode::Halt);
        return;
    }

    // Fallback: full table scan with Rewind→Next loop
    int32_t rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    int32_t loop_start = program_.current_pos();

    if (node.where_clause)
    {
        compile_expr(node.where_clause);
        int32_t skip_jump = program_.emit(OpCode::JumpIfFalse, 0);

        for (const auto & assign : node.assignments)
        {
            int32_t col_idx = find_column_index(cursor, assign.column_name);
            if (col_idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found", assign.column_name));
            }
            program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, col_idx)));
            compile_expr(assign.value);
        }
        program_.emit(OpCode::UpdateRow, cursor, static_cast<int32_t>(node.assignments.size()));

        int32_t next_pos = program_.current_pos();
        program_.patch_jump(skip_jump, next_pos);
    }
    else
    {
        for (const auto & assign : node.assignments)
        {
            int32_t col_idx = find_column_index(cursor, assign.column_name);
            if (col_idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found", assign.column_name));
            }
            program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, col_idx)));
            compile_expr(assign.value);
        }
        program_.emit(OpCode::UpdateRow, cursor, static_cast<int32_t>(node.assignments.size()));
    }

    int32_t next_pos = program_.current_pos();
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_pos);
    program_.emit(OpCode::Close, cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_delete(const neon::planner::DeleteNode & node)
{
    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::OpenWrite, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(node.table_name);
    if (qr && *qr)
    {
        cursor_schemas_[cursor] = *qr;
        table_to_cursor_[node.table_name] = cursor;
    }

    // Try PK-equality fast path: WHERE pk_col = const
    auto pk_eq = detect_pk_equality(node.where_clause, cursor);
    if (pk_eq.has_value())
    {
        auto [_, pk_col_idx, pk_value] = pk_eq.value();
        program_.emit(OpCode::Const, program_.add_constant(pk_value));
        int32_t seek_inst = program_.emit(OpCode::SeekByPk, cursor, 0);
        program_.emit(OpCode::DeleteRow, cursor);
        int32_t end_pos = program_.current_pos();
        program_.patch_jump_operand2(seek_inst, end_pos);
        program_.emit(OpCode::Close, cursor);
        program_.emit(OpCode::Halt);
        return;
    }

    // Fallback: full table scan with Rewind->Next loop
    int32_t rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    int32_t loop_start = program_.current_pos();

    if (node.where_clause)
    {
        compile_expr(node.where_clause);
        int32_t skip_jump = program_.emit(OpCode::JumpIfFalse, 0);
        program_.emit(OpCode::DeleteRow, cursor);
        int32_t next_pos = program_.current_pos();
        program_.patch_jump(skip_jump, next_pos);
    }
    else
    {
        program_.emit(OpCode::DeleteRow, cursor);
    }

    int32_t next_pos = program_.current_pos();
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_pos);
    program_.emit(OpCode::Close, cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_create_table(const neon::planner::CreateTableNode & node)
{
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    int32_t cols_idx = program_.add_column_defs(node.columns);
    program_.emit(OpCode::CreateTable, table_name_idx, cols_idx);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_create_index(const neon::planner::CreateIndexNode & node)
{
    int32_t name_idx = program_.add_string_constant(node.index_name);
    int32_t table_idx = program_.add_string_constant(node.table_name);
    int32_t col_idx = program_.add_string_constant(node.column_name);
    if (node.unique)
    {
        name_idx = -name_idx - 1;
    }
    program_.emit(OpCode::CreateIndex, name_idx, table_idx, col_idx);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_drop_table(const neon::planner::DropTableNode & node)
{
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::DropTable, table_name_idx, node.if_exists ? 1 : 0);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_expr(const neon::parser::ExprPtr & expr)
{
    if (!expr)
        return;
    switch (expr->type)
    {
        case neon::parser::ExprType::Literal: {
            const auto & lit = static_cast<const neon::parser::LiteralExpr &>(*expr);
            int32_t idx = program_.add_constant(lit.value);
            program_.emit(OpCode::Const, idx);
            break;
        }
        case neon::parser::ExprType::ColumnRef: {
            const auto & col = static_cast<const neon::parser::ColumnRefExpr &>(*expr);
            auto [cursor_id, col_idx] = resolve_column(col.table_name, col.column_name);
            program_.emit(OpCode::Column, cursor_id, col_idx);
            break;
        }
        case neon::parser::ExprType::Star: {
            const auto & star = static_cast<const neon::parser::StarExpr &>(*expr);
            if (!star.table_name.empty())
            {
                auto it = table_to_cursor_.find(star.table_name);
                if (it != table_to_cursor_.end())
                {
                    emit_all_columns(it->second);
                }
            }
            else
            {
                for (const auto & [name, cursor_id] : table_to_cursor_)
                {
                    emit_all_columns(cursor_id);
                }
            }
            break;
        }
        case neon::parser::ExprType::BinaryOp: {
            const auto & bin = static_cast<const neon::parser::BinaryOpExpr &>(*expr);
            compile_expr(bin.left);
            compile_expr(bin.right);
            switch (bin.op)
            {
                case neon::parser::BinaryOpType::Add:
                    program_.emit(OpCode::Add);
                    break;
                case neon::parser::BinaryOpType::Sub:
                    program_.emit(OpCode::Sub);
                    break;
                case neon::parser::BinaryOpType::Mul:
                    program_.emit(OpCode::Mul);
                    break;
                case neon::parser::BinaryOpType::Div:
                    program_.emit(OpCode::Div);
                    break;
                case neon::parser::BinaryOpType::Eq:
                    program_.emit(OpCode::Eq);
                    break;
                case neon::parser::BinaryOpType::Ne:
                    program_.emit(OpCode::Ne);
                    break;
                case neon::parser::BinaryOpType::Lt:
                    program_.emit(OpCode::Lt);
                    break;
                case neon::parser::BinaryOpType::Le:
                    program_.emit(OpCode::Le);
                    break;
                case neon::parser::BinaryOpType::Gt:
                    program_.emit(OpCode::Gt);
                    break;
                case neon::parser::BinaryOpType::Ge:
                    program_.emit(OpCode::Ge);
                    break;
                case neon::parser::BinaryOpType::And:
                    program_.emit(OpCode::And);
                    break;
                case neon::parser::BinaryOpType::Or:
                    program_.emit(OpCode::Or);
                    break;
            }
            break;
        }
        case neon::parser::ExprType::UnaryOp: {
            const auto & un = static_cast<const neon::parser::UnaryOpExpr &>(*expr);
            compile_expr(un.operand);
            switch (un.op)
            {
                case neon::parser::UnaryOpType::Not:
                    program_.emit(OpCode::Not);
                    break;
                case neon::parser::UnaryOpType::Neg:
                    program_.emit(OpCode::Neg);
                    break;
            }
            break;
        }
        case neon::parser::ExprType::FunctionCall: {
            const auto & func = static_cast<const neon::parser::FunctionCallExpr &>(*expr);
            if (in_agg_step_context_)
            {
                auto it = agg_expr_to_index_.find(expr.get());
                int32_t agg_idx = (it != agg_expr_to_index_.end()) ? it->second : 0;
                if (!func.args.empty())
                {
                    compile_expr(func.args[0]);
                }
                else
                {
                    program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, 1)));
                }
                program_.emit(OpCode::AggStep, agg_idx, static_cast<int32_t>(func.func));
            }
            else
            {
                auto it = agg_expr_to_index_.find(expr.get());
                int32_t agg_idx = (it != agg_expr_to_index_.end()) ? it->second : 0;
                program_.emit(OpCode::AggResult, agg_idx);
            }
            break;
        }
    }
}

std::pair<int32_t, int32_t> Compiler::resolve_column(const std::string & table_name, const std::string & col_name)
{
    if (!table_name.empty())
    {
        auto it = table_to_cursor_.find(table_name);
        if (it != table_to_cursor_.end())
        {
            int32_t col_idx = find_column_index(it->second, col_name);
            if (col_idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found in table '{}'", col_name, table_name));
            }
            return {it->second, col_idx};
        }
        throw std::runtime_error(std::format("Table '{}' not found", table_name));
    }
    else
    {
        for (const auto & [name, cursor_id] : table_to_cursor_)
        {
            int32_t col_idx = find_column_index(cursor_id, col_name);
            if (col_idx >= 0)
            {
                return {cursor_id, col_idx};
            }
        }
        throw std::runtime_error(std::format("Column '{}' not found", col_name));
    }
}

int32_t Compiler::find_column_index(int32_t cursor_id, const std::string & col_name)
{
    auto it = cursor_schemas_.find(cursor_id);
    if (it != cursor_schemas_.end() && it->second)
    {
        for (size_t i = 0; i < it->second->columns_.size(); ++i)
        {
            if (it->second->columns_[i].column_name_ == col_name)
            {
                return static_cast<int32_t>(i);
            }
        }
    }
    return -1;
}

std::optional<std::tuple<int32_t, int32_t, neon::type::Value>>
Compiler::detect_pk_equality(const neon::parser::ExprPtr & where, int32_t cursor_id)
{
    if (!where)
        return std::nullopt;
    if (where->type != neon::parser::ExprType::BinaryOp)
        return std::nullopt;

    const auto & bin = static_cast<const neon::parser::BinaryOpExpr &>(*where);
    if (bin.op != neon::parser::BinaryOpType::Eq)
        return std::nullopt;
    if (!bin.left || !bin.right)
        return std::nullopt;

    // Check left side is ColumnRef matching PK column
    if (bin.left->type != neon::parser::ExprType::ColumnRef)
        return std::nullopt;
    const auto & col = static_cast<const neon::parser::ColumnRefExpr &>(*bin.left);

    auto it = cursor_schemas_.find(cursor_id);
    if (it == cursor_schemas_.end() || !it->second)
        return std::nullopt;

    int32_t pk_idx = it->second->primary_key_col_idx_;
    if (pk_idx < 0)
        return std::nullopt;

    // Resolve column to check it matches PK index
    int32_t col_idx = find_column_index(cursor_id, col.column_name);
    if (col_idx != pk_idx)
        return std::nullopt;

    // Check right side is a literal constant
    if (bin.right->type != neon::parser::ExprType::Literal)
        return std::nullopt;
    const auto & lit = static_cast<const neon::parser::LiteralExpr &>(*bin.right);

    return std::make_tuple(cursor_id, pk_idx, lit.value);
}

namespace
{

struct ColCmp
{
    std::string column;
    neon::parser::BinaryOpType op = neon::parser::BinaryOpType::Eq;
    neon::type::Value literal;
};

std::optional<ColCmp> column_cmp(const neon::parser::ExprPtr & expr)
{
    if (!expr || expr->type != neon::parser::ExprType::BinaryOp)
    {
        return std::nullopt;
    }
    const auto & bin = static_cast<const neon::parser::BinaryOpExpr &>(*expr);
    const neon::parser::Expr * left = bin.left.get();
    const neon::parser::Expr * right = bin.right.get();
    if (left == nullptr || right == nullptr)
    {
        return std::nullopt;
    }
    auto flip = [](neon::parser::BinaryOpType op) {
        using neon::parser::BinaryOpType;
        switch (op)
        {
            case BinaryOpType::Lt:
                return BinaryOpType::Gt;
            case BinaryOpType::Le:
                return BinaryOpType::Ge;
            case BinaryOpType::Gt:
                return BinaryOpType::Lt;
            case BinaryOpType::Ge:
                return BinaryOpType::Le;
            default:
                return op;
        }
    };
    ColCmp out;
    if (left->type == neon::parser::ExprType::ColumnRef && right->type == neon::parser::ExprType::Literal)
    {
        out.column = static_cast<const neon::parser::ColumnRefExpr &>(*left).column_name;
        out.op = bin.op;
        out.literal = static_cast<const neon::parser::LiteralExpr &>(*right).value;
        return out;
    }
    if (right->type == neon::parser::ExprType::ColumnRef && left->type == neon::parser::ExprType::Literal)
    {
        out.column = static_cast<const neon::parser::ColumnRefExpr &>(*right).column_name;
        out.op = flip(bin.op);
        out.literal = static_cast<const neon::parser::LiteralExpr &>(*left).value;
        return out;
    }
    return std::nullopt;
}

} // namespace

std::optional<Compiler::KeyBounds> Compiler::detect_pk_bounds(const neon::parser::ExprPtr & where, int32_t cursor_id)
{
    auto schema_it = cursor_schemas_.find(cursor_id);
    if (schema_it == cursor_schemas_.end() || !schema_it->second || schema_it->second->primary_key_col_idx_ < 0)
    {
        return std::nullopt;
    }
    int32_t pk_idx = schema_it->second->primary_key_col_idx_;
    KeyBounds bounds;
    bool any = false;
    auto absorb = [&](auto && self, const neon::parser::ExprPtr & expr) -> bool {
        if (!expr)
        {
            return false;
        }
        if (expr->type == neon::parser::ExprType::BinaryOp)
        {
            const auto & bin = static_cast<const neon::parser::BinaryOpExpr &>(*expr);
            if (bin.op == neon::parser::BinaryOpType::And)
            {
                return self(self, bin.left) && self(self, bin.right);
            }
        }
        auto cmp = column_cmp(expr);
        if (!cmp || find_column_index(cursor_id, cmp->column) != pk_idx)
        {
            return false;
        }
        using neon::parser::BinaryOpType;
        switch (cmp->op)
        {
            case BinaryOpType::Gt:
                if (bounds.lower)
                    return false;
                bounds.lower = cmp->literal;
                bounds.lower_inclusive = false;
                break;
            case BinaryOpType::Ge:
                if (bounds.lower)
                    return false;
                bounds.lower = cmp->literal;
                bounds.lower_inclusive = true;
                break;
            case BinaryOpType::Lt:
                if (bounds.upper)
                    return false;
                bounds.upper = cmp->literal;
                bounds.upper_inclusive = false;
                break;
            case BinaryOpType::Le:
                if (bounds.upper)
                    return false;
                bounds.upper = cmp->literal;
                bounds.upper_inclusive = true;
                break;
            default:
                return false;
        }
        any = true;
        return true;
    };
    if (!absorb(absorb, where) || !any)
    {
        return std::nullopt;
    }
    return bounds;
}

std::optional<std::pair<int32_t, neon::type::Value>> Compiler::detect_index_equality(const neon::parser::ExprPtr & where, int32_t cursor_id)
{
    auto cmp = column_cmp(where);
    if (!cmp || cmp->op != neon::parser::BinaryOpType::Eq)
    {
        return std::nullopt;
    }
    auto schema_it = cursor_schemas_.find(cursor_id);
    if (schema_it == cursor_schemas_.end() || !schema_it->second)
    {
        return std::nullopt;
    }
    int32_t col = find_column_index(cursor_id, cmp->column);
    if (col < 0 || col == schema_it->second->primary_key_col_idx_)
    {
        return std::nullopt;
    }
    const auto & indexes = schema_it->second->indexes_;
    for (size_t i = 0; i < indexes.size(); ++i)
    {
        if (indexes[i].index_cols_.size() == 1 && indexes[i].index_cols_[0] == col)
        {
            return std::make_pair(static_cast<int32_t>(i), cmp->literal);
        }
    }
    return std::nullopt;
}

void Compiler::emit_all_columns(int32_t cursor_id)
{
    auto it = cursor_schemas_.find(cursor_id);
    if (it != cursor_schemas_.end() && it->second)
    {
        for (size_t i = 0; i < it->second->columns_.size(); ++i)
        {
            program_.emit(OpCode::Column, cursor_id, static_cast<int32_t>(i));
        }
    }
}

void Compiler::open_table_cursor(const std::string & table_name, bool for_write)
{
    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(table_name);
    if (for_write)
    {
        program_.emit(OpCode::OpenWrite, cursor, table_name_idx);
    }
    else
    {
        program_.emit(OpCode::OpenRead, cursor, table_name_idx);
    }
    auto qr = schema_mgr_.query_table(table_name);
    if (qr && *qr)
    {
        cursor_schemas_[cursor] = *qr;
        table_to_cursor_[table_name] = cursor;
    }
}

void Compiler::compile_select(const neon::planner::ProjectNode & project)
{
    SelectPipeline pipe;
    pipe.project = &project;
    analyze_select_pipeline(project.child, pipe);
    compile_select_pipeline(pipe);
}

void Compiler::analyze_select_pipeline(const neon::planner::PlanNodePtr & node, SelectPipeline & pipe)
{
    if (!node)
        return;
    switch (node->type)
    {
        case neon::planner::PlanNodeType::Filter: {
            const auto * f = static_cast<const neon::planner::FilterNode *>(node.get());
            analyze_select_pipeline(f->child, pipe);
            // A filter under the aggregate is WHERE. A filter above it is HAVING.
            if (pipe.agg)
            {
                pipe.having = f;
            }
            else
            {
                pipe.where_filters.push_back(f);
            }
            break;
        }
        case neon::planner::PlanNodeType::Agg: {
            const auto * agg = static_cast<const neon::planner::AggNode *>(node.get());
            analyze_select_pipeline(agg->child, pipe);
            pipe.agg = agg;
            break;
        }
        case neon::planner::PlanNodeType::Sort: {
            pipe.sort = static_cast<const neon::planner::SortNode *>(node.get());
            analyze_select_pipeline(pipe.sort->child, pipe);
            break;
        }
        case neon::planner::PlanNodeType::Limit: {
            pipe.limit = static_cast<const neon::planner::LimitNode *>(node.get());
            analyze_select_pipeline(pipe.limit->child, pipe);
            break;
        }
        case neon::planner::PlanNodeType::Scan: {
            pipe.scan = static_cast<const neon::planner::ScanNode *>(node.get());
            break;
        }
        case neon::planner::PlanNodeType::Join: {
            pipe.join = static_cast<const neon::planner::JoinNode *>(node.get());
            break;
        }
        default:
            break;
    }
}

void Compiler::compile_select_pipeline(const SelectPipeline & pipe)
{
    if (pipe.join)
    {
        compile_select_with_join(pipe);
    }
    else if (pipe.agg)
    {
        compile_select_with_agg(pipe);
    }
    else if (pipe.sort)
    {
        compile_select_with_sort(pipe);
    }
    else
    {
        compile_select_simple(pipe);
    }
}

int32_t Compiler::resolve_project_columns(const std::vector<neon::parser::ExprPtr> & exprs, std::vector<std::string> & out_names)
{
    out_names.clear();
    int32_t count = 0;
    for (const auto & expr : exprs)
    {
        if (expr->type == neon::parser::ExprType::Star)
        {
            // Expand star to all columns from all tables
            for (const auto & [table_name, cursor_id] : table_to_cursor_)
            {
                auto it = cursor_schemas_.find(cursor_id);
                if (it != cursor_schemas_.end() && it->second)
                {
                    for (const auto & col : it->second->columns_)
                    {
                        out_names.push_back(col.column_name_);
                        count++;
                    }
                }
            }
        }
        else if (expr->type == neon::parser::ExprType::ColumnRef)
        {
            const auto & col = static_cast<const neon::parser::ColumnRefExpr &>(*expr);
            out_names.push_back(col.column_name);
            count++;
        }
        else if (expr->type == neon::parser::ExprType::FunctionCall)
        {
            const auto & func = static_cast<const neon::parser::FunctionCallExpr &>(*expr);
            // Derive name from function call (e.g., COUNT(*), SUM(val))
            std::string fn_name;
            switch (func.func)
            {
                case neon::parser::AggFuncType::Count:
                    fn_name = "COUNT";
                    break;
                case neon::parser::AggFuncType::Sum:
                    fn_name = "SUM";
                    break;
                case neon::parser::AggFuncType::Avg:
                    fn_name = "AVG";
                    break;
                case neon::parser::AggFuncType::Min:
                    fn_name = "MIN";
                    break;
                case neon::parser::AggFuncType::Max:
                    fn_name = "MAX";
                    break;
            }
            if (!func.args.empty())
            {
                if (func.args[0]->type == neon::parser::ExprType::Star)
                {
                    out_names.push_back(fn_name + "(*)");
                }
                else if (func.args[0]->type == neon::parser::ExprType::ColumnRef)
                {
                    const auto & col_ref = static_cast<const neon::parser::ColumnRefExpr &>(*func.args[0]);
                    out_names.push_back(fn_name + "(" + col_ref.column_name + ")");
                }
                else
                {
                    out_names.push_back(fn_name + "(...)");
                }
            }
            else
            {
                out_names.push_back(fn_name + "()");
            }
            count++;
        }
        else
        {
            // Other expressions (BinaryOp, UnaryOp, Literal, etc.)
            // Use a generic name or the planner-provided name
            out_names.push_back("");
            count++;
        }
    }
    // Fill in any empty names from planner-provided output_names as fallback
    // (handled in compile_select which sets up ColumnName instructions)
    return count;
}

void Compiler::compile_select_simple(const SelectPipeline & pipe)
{
    if (!pipe.scan)
        return;

    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(pipe.scan->table_name);
    program_.emit(OpCode::OpenRead, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(pipe.scan->table_name);
    if (qr && *qr)
    {
        auto schema = *qr;
        cursor_schemas_[cursor] = schema;
        table_to_cursor_[pipe.scan->table_name] = cursor;
        if (!pipe.scan->alias.empty())
        {
            table_to_cursor_[pipe.scan->alias] = cursor;
        }
    }

    // Resolve actual column count and names (expand StarExpr) — must be after OpenRead
    // so that table_to_cursor_ is populated for star expansion
    std::vector<std::string> expanded_names;
    int32_t num_output_cols = 0;
    if (pipe.project)
    {
        num_output_cols = resolve_project_columns(pipe.project->expressions, expanded_names);
        for (size_t i = 0; i < expanded_names.size(); ++i)
        {
            int32_t name_idx = program_.add_string_constant(expanded_names[i]);
            program_.emit(OpCode::ColumnName, static_cast<int32_t>(i), name_idx);
        }
    }

    if (pipe.where_filters.size() == 1 && !pipe.having)
    {
        auto pk_eq = detect_pk_equality(pipe.where_filters[0]->condition, cursor);
        if (pk_eq.has_value())
        {
            auto pk_value = std::get<2>(pk_eq.value());
            program_.emit(OpCode::Const, program_.add_constant(pk_value));
            int32_t seek_inst = program_.emit(OpCode::SeekByPk, cursor, 0);
            if (pipe.project)
            {
                for (const auto & expr : pipe.project->expressions)
                {
                    compile_expr(expr);
                }
                program_.emit(OpCode::ResultRow, num_output_cols);
            }
            else
            {
                emit_all_columns(cursor);
                auto it = cursor_schemas_.find(cursor);
                int32_t num_cols = (it != cursor_schemas_.end() && it->second) ? static_cast<int32_t>(it->second->columns_.size()) : 0;
                program_.emit(OpCode::ResultRow, num_cols);
            }
            int32_t end_pos = program_.current_pos();
            program_.patch_jump_operand2(seek_inst, end_pos);
            program_.emit(OpCode::Close, cursor);
            program_.emit(OpCode::Halt);
            return;
        }

        auto indexed = detect_index_equality(pipe.where_filters[0]->condition, cursor);
        if (indexed.has_value())
        {
            program_.emit(OpCode::Const, program_.add_constant(indexed->second));
            int32_t seek_inst = program_.emit(OpCode::SeekIndex, cursor, 0, indexed->first);
            int32_t loop_start = program_.current_pos();
            if (pipe.project)
            {
                for (const auto & expr : pipe.project->expressions)
                {
                    compile_expr(expr);
                }
                program_.emit(OpCode::ResultRow, num_output_cols);
            }
            else
            {
                emit_all_columns(cursor);
                auto it = cursor_schemas_.find(cursor);
                int32_t num_cols = (it != cursor_schemas_.end() && it->second) ? static_cast<int32_t>(it->second->columns_.size()) : 0;
                program_.emit(OpCode::ResultRow, num_cols);
            }
            program_.emit(OpCode::Next, cursor, loop_start);
            int32_t end_pos = program_.current_pos();
            program_.patch_jump_operand2(seek_inst, end_pos);
            program_.emit(OpCode::Close, cursor);
            program_.emit(OpCode::Halt);
            return;
        }
    }

    std::optional<KeyBounds> bounds;
    if (pipe.where_filters.size() == 1 && !pipe.having)
    {
        bounds = detect_pk_bounds(pipe.where_filters[0]->condition, cursor);
    }

    if (pipe.limit)
    {
        program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int64, pipe.limit->limit_count)));
    }

    int32_t rewind_inst = 0;
    if (bounds && bounds->lower)
    {
        program_.emit(OpCode::Const, program_.add_constant(*bounds->lower));
        rewind_inst = program_.emit(OpCode::SeekGe, cursor, 0);
    }
    else
    {
        rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    }
    int32_t loop_start = program_.current_pos();

    std::vector<int32_t> filter_jumps;
    int32_t stop_jump = -1;
    if (bounds)
    {
        auto schema_it = cursor_schemas_.find(cursor);
        int32_t pk_idx = (schema_it != cursor_schemas_.end() && schema_it->second) ? schema_it->second->primary_key_col_idx_ : 0;
        if (bounds->upper)
        {
            program_.emit(OpCode::Column, cursor, pk_idx);
            program_.emit(OpCode::Const, program_.add_constant(*bounds->upper));
            program_.emit(bounds->upper_inclusive ? OpCode::Gt : OpCode::Ge);
            stop_jump = program_.emit(OpCode::JumpIfTrue, 0);
        }
        if (bounds->lower && !bounds->lower_inclusive)
        {
            program_.emit(OpCode::Column, cursor, pk_idx);
            program_.emit(OpCode::Const, program_.add_constant(*bounds->lower));
            program_.emit(OpCode::Gt);
            filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
        }
    }
    else
    {
        for (const auto * f : pipe.where_filters)
        {
            compile_expr(f->condition);
            filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
        }
    }

    if (pipe.having)
    {
        compile_expr(pipe.having->condition);
        filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
    }

    if (pipe.project)
    {
        for (const auto & expr : pipe.project->expressions)
        {
            compile_expr(expr);
        }
        program_.emit(OpCode::ResultRow, num_output_cols);
    }
    else
    {
        emit_all_columns(cursor);
        auto it = cursor_schemas_.find(cursor);
        int32_t num_cols = (it != cursor_schemas_.end() && it->second) ? static_cast<int32_t>(it->second->columns_.size()) : 0;
        program_.emit(OpCode::ResultRow, num_cols);
    }

    int32_t limit_jump = -1;
    if (pipe.limit)
    {
        program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, 1)));
        program_.emit(OpCode::Sub);
        program_.emit(OpCode::Dup);
        limit_jump = program_.emit(OpCode::JumpIfFalse, 0);
    }

    int32_t next_pos = program_.current_pos();
    for (auto jmp : filter_jumps)
    {
        program_.patch_jump(jmp, next_pos);
    }
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_pos);
    if (stop_jump >= 0)
    {
        program_.patch_jump(stop_jump, end_pos);
    }
    if (limit_jump >= 0)
    {
        program_.patch_jump(limit_jump, end_pos);
    }
    program_.emit(OpCode::Close, cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_select_with_sort(const SelectPipeline & pipe)
{
    if (!pipe.scan || !pipe.sort)
        return;

    int32_t sorter = alloc_sorter();
    int32_t num_sort_keys = static_cast<int32_t>(pipe.sort->order_by.size());
    program_.emit(OpCode::SorterOpen, sorter, num_sort_keys);

    for (int32_t i = 0; i < num_sort_keys; ++i)
    {
        int32_t ascending = pipe.sort->order_by[i].ascending ? 1 : 0;
        program_.emit(OpCode::SortKeyDef, sorter, i, ascending);
    }

    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(pipe.scan->table_name);
    program_.emit(OpCode::OpenRead, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(pipe.scan->table_name);
    if (qr && *qr)
    {
        auto schema = *qr;
        cursor_schemas_[cursor] = schema;
        table_to_cursor_[pipe.scan->table_name] = cursor;
        if (!pipe.scan->alias.empty())
        {
            table_to_cursor_[pipe.scan->alias] = cursor;
        }
    }

    // Resolve actual column count and names (expand StarExpr) — must be after OpenRead
    // so that table_to_cursor_ is populated for star expansion
    std::vector<std::string> expanded_names;
    int32_t num_project_cols = 0;
    if (pipe.project)
    {
        num_project_cols = resolve_project_columns(pipe.project->expressions, expanded_names);
        for (size_t i = 0; i < expanded_names.size(); ++i)
        {
            int32_t name_idx = program_.add_string_constant(expanded_names[i]);
            program_.emit(OpCode::ColumnName, static_cast<int32_t>(i), name_idx);
        }
    }

    int32_t rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    int32_t loop_start = program_.current_pos();

    std::vector<int32_t> filter_jumps;
    for (const auto * f : pipe.where_filters)
    {
        compile_expr(f->condition);
        filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
    }

    for (const auto & order : pipe.sort->order_by)
    {
        compile_expr(order.expr);
    }

    if (pipe.project)
    {
        for (const auto & expr : pipe.project->expressions)
        {
            compile_expr(expr);
        }
    }
    else
    {
        emit_all_columns(cursor);
        auto it = cursor_schemas_.find(cursor);
        num_project_cols = (it != cursor_schemas_.end() && it->second) ? static_cast<int32_t>(it->second->columns_.size()) : 0;
    }

    program_.emit(OpCode::SorterInsert, sorter, num_sort_keys + num_project_cols);

    int32_t next_pos = program_.current_pos();
    for (auto jmp : filter_jumps)
    {
        program_.patch_jump(jmp, next_pos);
    }
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_scan_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_scan_pos);
    program_.emit(OpCode::Close, cursor);

    if (pipe.limit)
    {
        program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int64, pipe.limit->limit_count)));
    }

    program_.emit(OpCode::SorterSort, sorter);
    int32_t rewind_sorted = program_.emit(OpCode::SorterRewind, sorter, 0);
    int32_t sorted_loop = program_.current_pos();

    for (int32_t i = 0; i < num_project_cols; ++i)
    {
        program_.emit(OpCode::SorterColumn, sorter, num_sort_keys + i);
    }
    if (num_project_cols > 0)
    {
        program_.emit(OpCode::ResultRow, num_project_cols);
    }

    int32_t limit_jump = -1;
    if (pipe.limit)
    {
        program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, 1)));
        program_.emit(OpCode::Sub);
        program_.emit(OpCode::Dup);
        limit_jump = program_.emit(OpCode::JumpIfFalse, 0);
    }

    program_.emit(OpCode::SorterNext, sorter, sorted_loop);

    int32_t end_sorted_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_sorted, end_sorted_pos);
    if (limit_jump >= 0)
    {
        program_.patch_jump(limit_jump, end_sorted_pos);
    }
    program_.emit(OpCode::Halt);
}

void Compiler::compile_select_with_agg(const SelectPipeline & pipe)
{
    if (!pipe.agg || !pipe.scan)
        return;

    agg_expr_to_index_.clear();
    for (size_t i = 0; i < pipe.agg->agg_exprs.size(); ++i)
    {
        agg_expr_to_index_[pipe.agg->agg_exprs[i].get()] = static_cast<int32_t>(i);
    }

    program_.emit(OpCode::AggInit, static_cast<int32_t>(pipe.agg->agg_exprs.size()));

    int32_t cursor = alloc_cursor();
    int32_t table_name_idx = program_.add_string_constant(pipe.scan->table_name);
    program_.emit(OpCode::OpenRead, cursor, table_name_idx);

    auto qr = schema_mgr_.query_table(pipe.scan->table_name);
    if (qr && *qr)
    {
        auto schema = *qr;
        cursor_schemas_[cursor] = schema;
        table_to_cursor_[pipe.scan->table_name] = cursor;
        if (!pipe.scan->alias.empty())
        {
            table_to_cursor_[pipe.scan->alias] = cursor;
        }
    }

    // Resolve actual column count and names (expand StarExpr) — must be after OpenRead
    // so that table_to_cursor_ is populated for star expansion
    std::vector<std::string> expanded_names;
    int32_t num_output_cols = 0;
    if (pipe.project)
    {
        num_output_cols = resolve_project_columns(pipe.project->expressions, expanded_names);
        for (size_t i = 0; i < expanded_names.size(); ++i)
        {
            int32_t name_idx = program_.add_string_constant(expanded_names[i]);
            program_.emit(OpCode::ColumnName, static_cast<int32_t>(i), name_idx);
        }
    }

    bool plain_count_star = pipe.where_filters.empty() && !pipe.having && pipe.agg->agg_exprs.size() == 1
        && pipe.agg->agg_exprs[0] && pipe.agg->agg_exprs[0]->type == neon::parser::ExprType::FunctionCall;
    if (plain_count_star)
    {
        const auto & func = static_cast<const neon::parser::FunctionCallExpr &>(*pipe.agg->agg_exprs[0]);
        bool count_star = func.args.empty()
            || (func.args.size() == 1 && func.args[0] && func.args[0]->type == neon::parser::ExprType::Star);
        if (func.func == neon::parser::AggFuncType::Count && count_star)
        {
            program_.emit(OpCode::TableCount, cursor);
            program_.emit(OpCode::ResultRow, 1);
            program_.emit(OpCode::Close, cursor);
            program_.emit(OpCode::Halt);
            return;
        }
    }

    int32_t rewind_inst = program_.emit(OpCode::Rewind, cursor, 0);
    int32_t loop_start = program_.current_pos();

    std::vector<int32_t> filter_jumps;
    for (const auto * f : pipe.where_filters)
    {
        compile_expr(f->condition);
        filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
    }

    in_agg_step_context_ = true;
    for (size_t i = 0; i < pipe.agg->agg_exprs.size(); ++i)
    {
        const auto & expr = pipe.agg->agg_exprs[i];
        if (expr->type == neon::parser::ExprType::FunctionCall)
        {
            const auto & func = static_cast<const neon::parser::FunctionCallExpr &>(*expr);
            if (!func.args.empty())
            {
                compile_expr(func.args[0]);
            }
            else
            {
                program_.emit(OpCode::Const, program_.add_constant(neon::type::Value(neon::type::ValueType::Int32, 1)));
            }
            program_.emit(OpCode::AggStep, static_cast<int32_t>(i), static_cast<int32_t>(func.func));
        }
    }
    in_agg_step_context_ = false;

    int32_t next_pos = program_.current_pos();
    for (auto jmp : filter_jumps)
    {
        program_.patch_jump(jmp, next_pos);
    }
    program_.emit(OpCode::Next, cursor, loop_start);

    int32_t end_pos = program_.current_pos();
    program_.patch_jump_operand2(rewind_inst, end_pos);
    program_.emit(OpCode::Close, cursor);

    if (pipe.project)
    {
        for (const auto & expr : pipe.project->expressions)
        {
            compile_expr(expr);
        }
        program_.emit(OpCode::ResultRow, num_output_cols);
    }
    else
    {
        for (size_t i = 0; i < pipe.agg->agg_exprs.size(); ++i)
        {
            program_.emit(OpCode::AggResult, static_cast<int32_t>(i));
        }
        program_.emit(OpCode::ResultRow, static_cast<int32_t>(pipe.agg->agg_exprs.size()));
    }

    program_.emit(OpCode::Halt);
}

void Compiler::compile_select_with_join(const SelectPipeline & pipe)
{
    if (!pipe.join)
        return;

    int32_t left_cursor = alloc_cursor();
    const neon::planner::ScanNode * left_scan = nullptr;
    if (pipe.join->left && pipe.join->left->type == neon::planner::PlanNodeType::Scan)
    {
        left_scan = static_cast<const neon::planner::ScanNode *>(pipe.join->left.get());
    }
    if (!left_scan)
    {
        return;
    }

    int32_t left_table_idx = program_.add_string_constant(left_scan->table_name);
    program_.emit(OpCode::OpenRead, left_cursor, left_table_idx);
    neon::catalog::SchemaRef left_schema;
    auto qr_left = schema_mgr_.query_table(left_scan->table_name);
    if (qr_left && *qr_left)
    {
        left_schema = *qr_left;
        cursor_schemas_[left_cursor] = left_schema;
        table_to_cursor_[left_scan->table_name] = left_cursor;
        if (!left_scan->alias.empty())
        {
            table_to_cursor_[left_scan->alias] = left_cursor;
        }
    }

    int32_t right_cursor = alloc_cursor();
    const neon::planner::ScanNode * right_scan = nullptr;
    if (pipe.join->right && pipe.join->right->type == neon::planner::PlanNodeType::Scan)
    {
        right_scan = static_cast<const neon::planner::ScanNode *>(pipe.join->right.get());
    }
    if (!right_scan)
    {
        return;
    }

    int32_t right_table_idx = program_.add_string_constant(right_scan->table_name);
    program_.emit(OpCode::OpenRead, right_cursor, right_table_idx);
    neon::catalog::SchemaRef right_schema;
    auto qr_right = schema_mgr_.query_table(right_scan->table_name);
    if (qr_right && *qr_right)
    {
        right_schema = *qr_right;
        cursor_schemas_[right_cursor] = right_schema;
        table_to_cursor_[right_scan->table_name] = right_cursor;
        if (!right_scan->alias.empty())
        {
            table_to_cursor_[right_scan->alias] = right_cursor;
        }
    }

    // Resolve actual column count and names (expand StarExpr) — must be after OpenRead
    // so that table_to_cursor_ is populated for star expansion
    std::vector<std::string> expanded_names;
    int32_t num_output_cols = 0;
    if (pipe.project)
    {
        num_output_cols = resolve_project_columns(pipe.project->expressions, expanded_names);
        for (size_t i = 0; i < expanded_names.size(); ++i)
        {
            int32_t name_idx = program_.add_string_constant(expanded_names[i]);
            program_.emit(OpCode::ColumnName, static_cast<int32_t>(i), name_idx);
        }
    }

    // Equijoin on a primary key: scan the other side and probe with SeekByPk.
    if (pipe.join->join_type == neon::parser::JoinType::Inner && pipe.join->condition
        && pipe.join->condition->type == neon::parser::ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const neon::parser::BinaryOpExpr &>(*pipe.join->condition);
        if (bin.op == neon::parser::BinaryOpType::Eq && bin.left && bin.right && bin.left->type == neon::parser::ExprType::ColumnRef
            && bin.right->type == neon::parser::ExprType::ColumnRef)
        {
            const auto & key_left = static_cast<const neon::parser::ColumnRefExpr &>(*bin.left);
            const auto & key_right = static_cast<const neon::parser::ColumnRefExpr &>(*bin.right);
            auto [c0, i0] = resolve_column(key_left.table_name, key_left.column_name);
            auto [c1, i1] = resolve_column(key_right.table_name, key_right.column_name);
            auto pk_index = [this](int32_t cursor) -> int32_t {
                auto it = cursor_schemas_.find(cursor);
                if (it == cursor_schemas_.end() || !it->second)
                {
                    return -1;
                }
                return it->second->primary_key_col_idx_;
            };
            int32_t outer_cursor = -1;
            int32_t inner_cursor = -1;
            int32_t outer_col = -1;
            if (i0 >= 0 && i0 == pk_index(c0) && c0 != c1)
            {
                inner_cursor = c0;
                outer_cursor = c1;
                outer_col = i1;
            }
            else if (i1 >= 0 && i1 == pk_index(c1) && c0 != c1)
            {
                inner_cursor = c1;
                outer_cursor = c0;
                outer_col = i0;
            }
            if (outer_cursor >= 0)
            {
                int32_t rewind_inst = program_.emit(OpCode::Rewind, outer_cursor, 0);
                int32_t loop_start = program_.current_pos();
                program_.emit(OpCode::Column, outer_cursor, outer_col);
                int32_t seek_inst = program_.emit(OpCode::SeekByPk, inner_cursor, 0);
                std::vector<int32_t> filter_jumps;
                for (const auto * f : pipe.where_filters)
                {
                    compile_expr(f->condition);
                    filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
                }
                if (pipe.project)
                {
                    for (const auto & expr : pipe.project->expressions)
                    {
                        compile_expr(expr);
                    }
                    program_.emit(OpCode::ResultRow, num_output_cols);
                }
                else
                {
                    emit_all_columns(left_cursor);
                    emit_all_columns(right_cursor);
                    int32_t num_cols = 0;
                    if (left_schema)
                    {
                        num_cols += static_cast<int32_t>(left_schema->columns_.size());
                    }
                    if (right_schema)
                    {
                        num_cols += static_cast<int32_t>(right_schema->columns_.size());
                    }
                    program_.emit(OpCode::ResultRow, num_cols);
                }
                int32_t next_pos = program_.current_pos();
                program_.patch_jump_operand2(seek_inst, next_pos);
                for (auto jmp : filter_jumps)
                {
                    program_.patch_jump(jmp, next_pos);
                }
                program_.emit(OpCode::Next, outer_cursor, loop_start);
                int32_t end_pos = program_.current_pos();
                program_.patch_jump_operand2(rewind_inst, end_pos);
                program_.emit(OpCode::Close, right_cursor);
                program_.emit(OpCode::Close, left_cursor);
                program_.emit(OpCode::Halt);
                return;
            }
        }
    }

    int32_t left_rewind = program_.emit(OpCode::Rewind, left_cursor, 0);
    int32_t left_loop = program_.current_pos();

    int32_t right_rewind = program_.emit(OpCode::Rewind, right_cursor, 0);
    int32_t right_loop = program_.current_pos();

    int32_t join_cond_jump = -1;
    if (pipe.join->condition)
    {
        compile_expr(pipe.join->condition);
        join_cond_jump = program_.emit(OpCode::JumpIfFalse, 0);
    }

    std::vector<int32_t> filter_jumps;
    for (const auto * f : pipe.where_filters)
    {
        compile_expr(f->condition);
        filter_jumps.push_back(program_.emit(OpCode::JumpIfFalse, 0));
    }

    if (pipe.project)
    {
        for (const auto & expr : pipe.project->expressions)
        {
            compile_expr(expr);
        }
        program_.emit(OpCode::ResultRow, num_output_cols);
    }
    else
    {
        emit_all_columns(left_cursor);
        emit_all_columns(right_cursor);
        int32_t num_cols = 0;
        if (left_schema)
            num_cols += static_cast<int32_t>(left_schema->columns_.size());
        if (right_schema)
            num_cols += static_cast<int32_t>(right_schema->columns_.size());
        program_.emit(OpCode::ResultRow, num_cols);
    }

    int32_t right_next_pos = program_.current_pos();
    if (join_cond_jump >= 0)
    {
        program_.patch_jump(join_cond_jump, right_next_pos);
    }
    for (auto jmp : filter_jumps)
    {
        program_.patch_jump(jmp, right_next_pos);
    }
    program_.emit(OpCode::Next, right_cursor, right_loop);

    int32_t right_end = program_.current_pos();
    program_.patch_jump_operand2(right_rewind, right_end);
    program_.emit(OpCode::Next, left_cursor, left_loop);

    int32_t left_end = program_.current_pos();
    program_.patch_jump_operand2(left_rewind, left_end);
    program_.emit(OpCode::Close, right_cursor);
    program_.emit(OpCode::Close, left_cursor);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_show_tables(const neon::planner::ShowTablesNode & /*node*/)
{
    int32_t col_name_idx = program_.add_string_constant("Tables_in_neon");
    program_.emit(OpCode::ColumnName, 0, col_name_idx);
    program_.emit(OpCode::ShowTables);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_describe(const neon::planner::DescribeNode & node)
{
    // Column names for DESCRIBE output
    int32_t field_idx = program_.add_string_constant("Field");
    int32_t type_idx = program_.add_string_constant("Type");
    int32_t null_idx = program_.add_string_constant("Null");
    int32_t key_idx = program_.add_string_constant("Key");
    int32_t default_idx = program_.add_string_constant("Default");
    int32_t extra_idx = program_.add_string_constant("Extra");
    program_.emit(OpCode::ColumnName, 0, field_idx);
    program_.emit(OpCode::ColumnName, 1, type_idx);
    program_.emit(OpCode::ColumnName, 2, null_idx);
    program_.emit(OpCode::ColumnName, 3, key_idx);
    program_.emit(OpCode::ColumnName, 4, default_idx);
    program_.emit(OpCode::ColumnName, 5, extra_idx);
    int32_t table_name_idx = program_.add_string_constant(node.table_name);
    program_.emit(OpCode::Describe, table_name_idx);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_show_databases(const neon::planner::ShowDatabasesNode & /*node*/)
{
    int32_t col_name_idx = program_.add_string_constant("Database");
    program_.emit(OpCode::ColumnName, 0, col_name_idx);
    program_.emit(OpCode::ShowDatabases);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_set_var(const neon::planner::SetVarNode & /*node*/)
{
    program_.emit(OpCode::SetVar);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_show_variables(const neon::planner::ShowVariablesNode & /*node*/)
{
    int32_t name_idx = program_.add_string_constant("Variable_name");
    int32_t value_idx = program_.add_string_constant("Value");
    program_.emit(OpCode::ColumnName, 0, name_idx);
    program_.emit(OpCode::ColumnName, 1, value_idx);
    program_.emit(OpCode::ShowVariables);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_show_status(const neon::planner::ShowStatusNode & /*node*/)
{
    int32_t name_idx = program_.add_string_constant("Variable_name");
    int32_t value_idx = program_.add_string_constant("Value");
    program_.emit(OpCode::ColumnName, 0, name_idx);
    program_.emit(OpCode::ColumnName, 1, value_idx);
    program_.emit(OpCode::ShowVariables); // reuse same opcode as show variables
    program_.emit(OpCode::Halt);
}

void Compiler::compile_select_database(const neon::planner::SelectDatabaseNode & /*node*/)
{
    int32_t col_name_idx = program_.add_string_constant("DATABASE()");
    program_.emit(OpCode::ColumnName, 0, col_name_idx);
    program_.emit(OpCode::SelectDatabase);
    program_.emit(OpCode::Halt);
}

void Compiler::compile_select_system_var(const neon::planner::SelectSystemVarNode & node)
{
    int32_t col_name_idx = program_.add_string_constant("@@" + node.var_name);
    program_.emit(OpCode::ColumnName, 0, col_name_idx);
    int32_t var_name_idx = program_.add_string_constant(node.var_name);
    program_.emit(OpCode::SelectSystemVar, var_name_idx);
    program_.emit(OpCode::Halt);
}

} // namespace neon::bytecode
