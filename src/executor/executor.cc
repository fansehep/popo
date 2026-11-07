#include "executor/executor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace popo::executor
{

using popo::parser::Expr;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

namespace
{

bool is_null(const Value & v)
{
    return v.datatype() == ValueType::Null;
}

bool as_bool(const Value & v)
{
    switch (v.datatype())
    {
        case ValueType::Bool:
            return v.boolean();
        case ValueType::Null:
            return false;
        case ValueType::String:
            return !v.string_ref().empty();
        default:
            return v.is_numeric() && v.to_f64() != 0.0;
    }
}

bool is_floating(const Value & v)
{
    return v.datatype() == ValueType::Float32 || v.datatype() == ValueType::Float64;
}

int64_t to_i64(const Value & v)
{
    switch (v.datatype())
    {
        case ValueType::Int8:
            return v.i8();
        case ValueType::Int16:
            return v.i16();
        case ValueType::Int32:
            return v.i32();
        case ValueType::Int64:
            return v.i64();
        case ValueType::Uint8:
            return static_cast<int64_t>(v.u8());
        case ValueType::Uint16:
            return static_cast<int64_t>(v.u16());
        case ValueType::Uint32:
            return static_cast<int64_t>(v.u32());
        case ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        case ValueType::Bool:
            return v.boolean() ? 1 : 0;
        default:
            return static_cast<int64_t>(v.to_f64());
    }
}

Value make_bool(bool b)
{
    return Value(ValueType::Bool, b);
}

// SQL comparison semantics: NULL or incompatible types yield unknown, treated as false.
bool comparable(const Value & l, const Value & r)
{
    return !is_null(l) && !is_null(r) && (l.datatype() == r.datatype() || (l.is_numeric() && r.is_numeric()));
}

bool sql_eq(const Value & l, const Value & r)
{
    return comparable(l, r) && l == r;
}

bool sql_lt(const Value & l, const Value & r)
{
    return comparable(l, r) && l < r;
}

Value do_arithmetic(char op, const Value & l, const Value & r)
{
    if (is_null(l) || is_null(r))
        return Value();
    bool floating = is_floating(l) || is_floating(r);
    switch (op)
    {
        case '+':
            return floating ? Value(ValueType::Float64, l.to_f64() + r.to_f64()) : Value(ValueType::Int64, to_i64(l) + to_i64(r));
        case '-':
            return floating ? Value(ValueType::Float64, l.to_f64() - r.to_f64()) : Value(ValueType::Int64, to_i64(l) - to_i64(r));
        case '*':
            return floating ? Value(ValueType::Float64, l.to_f64() * r.to_f64()) : Value(ValueType::Int64, to_i64(l) * to_i64(r));
        case '/':
            if (r.to_f64() == 0.0)
                return Value();
            if (floating)
                return Value(ValueType::Float64, l.to_f64() / r.to_f64());
            return Value(ValueType::Int64, to_i64(l) / to_i64(r));
        default:
            throw std::runtime_error("unknown arithmetic operator");
    }
}

// SQL LIKE pattern matching: '%' matches any sequence, '_' matches one character.
bool like_match(const std::string & text, const std::string & pattern)
{
    size_t t = 0;
    size_t p = 0;
    size_t star = std::string::npos;
    size_t backtrack = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t]))
        {
            ++t;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '%')
        {
            star = p++;
            backtrack = t;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            t = ++backtrack;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
    {
        ++p;
    }
    return p == pattern.size();
}

std::string value_key(const Value & v)
{
    return std::to_string(static_cast<int32_t>(v.datatype())) + ":" + v.to_val_string();
}

std::string row_key(const Row & row)
{
    std::string key;
    for (const auto & v : row.values_)
    {
        key += value_key(v);
        key += '\x1f';
    }
    return key;
}

std::string agg_display_name(const popo::parser::FunctionCallExpr & func)
{
    static const char * kNames[] = {"COUNT", "SUM", "AVG", "MIN", "MAX", "XMLAGG"};
    std::string name = kNames[static_cast<int32_t>(func.func)];
    name += "(";
    if (!func.args.empty() && func.args[0]->type == ExprType::ColumnRef)
    {
        name += static_cast<const popo::parser::ColumnRefExpr &>(*func.args[0]).column_name;
    }
    else if (!func.args.empty() && func.args[0]->type != ExprType::Star)
    {
        name += "...";
    }
    else
    {
        name += "*";
    }
    name += ")";
    return name;
}

bool expr_has_new_syntax(const ExprPtr & expr);

bool stmt_has_new_syntax(const popo::parser::StmtPtr & stmt)
{
    if (!stmt)
        return false;
    if (stmt->type == popo::parser::StmtType::AlterTable || stmt->type == popo::parser::StmtType::CreateView
        || stmt->type == popo::parser::StmtType::DropView)
        return true;
    if (stmt->type == popo::parser::StmtType::Select)
    {
        const auto & sel = static_cast<const popo::parser::SelectStatement &>(*stmt);
        if (sel.distinct || sel.offset.has_value() || !sel.set_ops.empty() || !sel.with_clause.empty() || !sel.from)
            return true;
        for (const auto & cte : sel.with_clause)
        {
            if (stmt_has_new_syntax(cte.query))
                return true;
        }
        for (const auto & term : sel.set_ops)
        {
            if (stmt_has_new_syntax(term.query))
                return true;
        }
        std::function<bool(const popo::parser::TableRefPtr &)> check_ref = [&](const popo::parser::TableRefPtr & ref) -> bool
        {
            if (!ref)
                return false;
            if (ref->type == popo::parser::TableRefType::Derived)
            {
                return true;
            }
            if (ref->type == popo::parser::TableRefType::Join)
            {
                const auto & join = static_cast<const popo::parser::JoinTableRef &>(*ref);
                return join.join_type == popo::parser::JoinType::Full || check_ref(join.left) || check_ref(join.right)
                    || expr_has_new_syntax(join.condition);
            }
            return false;
        };
        if (check_ref(sel.from))
            return true;
        for (const auto & e : sel.select_list)
        {
            if (expr_has_new_syntax(e))
                return true;
        }
        if (expr_has_new_syntax(sel.where) || expr_has_new_syntax(sel.having))
            return true;
        for (const auto & e : sel.group_by)
        {
            if (expr_has_new_syntax(e))
                return true;
        }
        for (const auto & item : sel.order_by)
        {
            if (expr_has_new_syntax(item.expr))
                return true;
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Insert)
    {
        const auto & ins = static_cast<const popo::parser::InsertStatement &>(*stmt);
        for (const auto & row : ins.values)
        {
            for (const auto & e : row)
            {
                if (expr_has_new_syntax(e))
                    return true;
            }
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Update)
    {
        const auto & upd = static_cast<const popo::parser::UpdateStatement &>(*stmt);
        if (expr_has_new_syntax(upd.where))
            return true;
        for (const auto & assign : upd.assignments)
        {
            if (expr_has_new_syntax(assign.value))
                return true;
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Delete)
    {
        return expr_has_new_syntax(static_cast<const popo::parser::DeleteStatement &>(*stmt).where);
    }
    return false;
}

bool expr_has_new_syntax(const ExprPtr & expr)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case ExprType::In:
        case ExprType::Between:
        case ExprType::Like:
        case ExprType::IsNull:
        case ExprType::Case:
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
        case ExprType::Alias:
        case ExprType::DistinctFrom:
        case ExprType::Quantified:
        case ExprType::ScalarCall:
        case ExprType::Cast:
        case ExprType::Window:
            return true;
        case ExprType::BinaryOp: {
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            if (bin.op == popo::parser::BinaryOpType::Concat)
                return true;
            return expr_has_new_syntax(bin.left) || expr_has_new_syntax(bin.right);
        }
        case ExprType::UnaryOp:
            return expr_has_new_syntax(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case ExprType::FunctionCall: {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
            if (func.func == popo::parser::AggFuncType::XmlAgg)
                return true;
            for (const auto & arg : func.args)
            {
                if (expr_has_new_syntax(arg))
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

} // namespace

bool needs_generic_executor(const popo::parser::StmtPtr & stmt)
{
    return stmt_has_new_syntax(stmt);
}

bool plan_needs_generic_executor(const popo::planner::PlanNodePtr & node)
{
    if (!node)
        return false;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::AlterTable:
        case PlanNodeType::Distinct:
        case PlanNodeType::SetOp:
        case PlanNodeType::SubqueryScan:
        case PlanNodeType::RecursiveScan:
        case PlanNodeType::Window:
        case PlanNodeType::Dual:
        case PlanNodeType::CreateView:
        case PlanNodeType::DropView:
            return true;
        case PlanNodeType::Filter:
            return plan_needs_generic_executor(static_cast<const popo::planner::FilterNode &>(*node).child);
        case PlanNodeType::Project:
            return plan_needs_generic_executor(static_cast<const popo::planner::ProjectNode &>(*node).child);
        case PlanNodeType::Agg:
            return plan_needs_generic_executor(static_cast<const popo::planner::AggNode &>(*node).child);
        case PlanNodeType::Sort:
            return plan_needs_generic_executor(static_cast<const popo::planner::SortNode &>(*node).child);
        case PlanNodeType::Limit:
            return plan_needs_generic_executor(static_cast<const popo::planner::LimitNode &>(*node).child);
        case PlanNodeType::Join: {
            const auto & join = static_cast<const popo::planner::JoinNode &>(*node);
            return plan_needs_generic_executor(join.left) || plan_needs_generic_executor(join.right);
        }
        default:
            return false;
    }
}

size_t Executor::Batch::width() const
{
    size_t n = 0;
    for (const auto & s : schemas)
    {
        n += s.columns.size();
    }
    return n;
}

std::vector<std::string> Executor::Batch::flat_names() const
{
    std::vector<std::string> names;
    for (const auto & s : schemas)
    {
        names.insert(names.end(), s.columns.begin(), s.columns.end());
    }
    return names;
}

Executor::Executor(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine)
    : schema_mgr_(schema_mgr), engine_(engine), planner_(schema_mgr)
{
}

popo::base::Result<popo::bytecode::ExecuteSuccess>
Executor::execute(const popo::planner::PlanNodePtr & plan, popo::txn::Transaction * outer_txn)
{
    result_ = popo::bytecode::ExecuteSuccess();
    subquery_plans_.clear();
    materialized_.clear();
    recursive_inputs_.clear();
    own_txn_ = outer_txn == nullptr;
    try
    {
        txn_ = own_txn_ ? engine_.begin_txn() : outer_txn;
        switch (plan->type)
        {
            case popo::planner::PlanNodeType::Insert:
                exec_insert(static_cast<const popo::planner::InsertNode &>(*plan));
                break;
            case popo::planner::PlanNodeType::Update:
                exec_update(static_cast<const popo::planner::UpdateNode &>(*plan));
                break;
            case popo::planner::PlanNodeType::Delete:
                exec_delete(static_cast<const popo::planner::DeleteNode &>(*plan));
                break;
            case popo::planner::PlanNodeType::AlterTable:
                exec_alter_table(static_cast<const popo::planner::AlterTableNode &>(*plan));
                break;
            case popo::planner::PlanNodeType::CreateView:
                exec_create_view(static_cast<const popo::planner::CreateViewNode &>(*plan));
                break;
            case popo::planner::PlanNodeType::DropView:
                exec_drop_view(static_cast<const popo::planner::DropViewNode &>(*plan));
                break;
            default: {
                auto batch = exec_batch(plan, nullptr);
                result_.columns = batch.flat_names();
                for (auto & row : batch.rows)
                {
                    result_.rows.push_back(std::make_shared<Row>(std::move(row)));
                }
                break;
            }
        }
        if (own_txn_ && txn_)
        {
            engine_.commit_txn(txn_);
        }
        txn_ = nullptr;
    }
    catch (const std::exception & e)
    {
        if (txn_ && own_txn_)
        {
            engine_.abort_txn(txn_);
        }
        txn_ = nullptr;
        return std::unexpected(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), e.what()));
    }

    if (!result_.rows.empty())
    {
        result_.message = std::to_string(result_.rows.size()) + " rows in set";
    }
    else if (result_.rows_affected > 0)
    {
        result_.message = std::to_string(result_.rows_affected) + " rows affected";
    }
    else if (result_.message.empty())
    {
        result_.message = "OK";
    }
    return result_;
}

Executor::Batch Executor::exec_batch(const popo::planner::PlanNodePtr & node, const EvalEnv * outer)
{
    if (!node)
        return {};
    switch (node->type)
    {
        case popo::planner::PlanNodeType::Scan:
            return exec_scan(static_cast<const popo::planner::ScanNode &>(*node));
        case popo::planner::PlanNodeType::SubqueryScan:
            return exec_subquery_scan(static_cast<const popo::planner::SubqueryScanNode &>(*node));
        case popo::planner::PlanNodeType::Dual: {
            Batch batch;
            batch.rows.emplace_back();
            return batch;
        }
        case popo::planner::PlanNodeType::Filter:
            return exec_filter(static_cast<const popo::planner::FilterNode &>(*node), outer);
        case popo::planner::PlanNodeType::Project:
            return exec_project(static_cast<const popo::planner::ProjectNode &>(*node), outer);
        case popo::planner::PlanNodeType::Join:
            return exec_join(static_cast<const popo::planner::JoinNode &>(*node), outer);
        case popo::planner::PlanNodeType::Agg:
            return exec_agg(static_cast<const popo::planner::AggNode &>(*node), outer);
        case popo::planner::PlanNodeType::Sort:
            return exec_sort(static_cast<const popo::planner::SortNode &>(*node), outer);
        case popo::planner::PlanNodeType::Limit:
            return exec_limit(static_cast<const popo::planner::LimitNode &>(*node), outer);
        case popo::planner::PlanNodeType::Distinct:
            return exec_distinct(static_cast<const popo::planner::DistinctNode &>(*node), outer);
        case popo::planner::PlanNodeType::SetOp:
            return exec_set_op(static_cast<const popo::planner::SetOpNode &>(*node), outer);
        case popo::planner::PlanNodeType::Window:
            return exec_window(static_cast<const popo::planner::WindowNode &>(*node), outer);
        case popo::planner::PlanNodeType::RecursiveScan:
            return exec_recursive_scan(static_cast<const popo::planner::RecursiveScanNode &>(*node));
        default:
            throw std::runtime_error("unsupported plan node in executor");
    }
}

Executor::Batch Executor::exec_scan(const popo::planner::ScanNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", node.table_name));
    }
    const auto & schema = *qr;
    Batch batch;
    ColumnSchema cs;
    // An alias hides the base name, so emp.dept inside FROM emp e2 resolves outward.
    cs.table = node.alias.empty() ? node.table_name : node.alias;
    cs.alias = node.alias;
    for (const auto & col : schema->columns_)
    {
        cs.columns.push_back(col.column_name_);
    }
    batch.schemas.push_back(std::move(cs));

    auto * heap = engine_.get_table_heap(node.table_name);
    if (heap)
    {
        popo::storage::TableIterator iter(heap, heap->schema());
        for (iter.rewind(); iter.valid(); iter.next())
        {
            batch.rows.push_back(iter.current_row());
        }
    }
    return batch;
}

Executor::Batch Executor::exec_subquery_scan(const popo::planner::SubqueryScanNode & node)
{
    auto it = materialized_.find(node.child.get());
    if (it == materialized_.end())
    {
        Batch built = node.recursive ? exec_recursive(node) : exec_batch(node.child, nullptr);
        it = materialized_.emplace(node.child.get(), std::move(built)).first;
    }
    Batch batch;
    ColumnSchema cs;
    cs.table = node.alias;
    cs.alias = node.alias;
    cs.columns = node.column_names.empty() ? it->second.flat_names() : node.column_names;
    batch.schemas.push_back(std::move(cs));
    batch.rows = it->second.rows;
    return batch;
}

Executor::Batch Executor::exec_filter(const popo::planner::FilterNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    Batch out;
    out.schemas = batch.schemas;
    bool has_aggs = !batch.row_aggs.empty();
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        env.row = &batch.rows[i];
        env.aggs = has_aggs ? &batch.row_aggs[i] : nullptr;
        if (as_bool(eval(node.condition, env)))
        {
            out.rows.push_back(std::move(batch.rows[i]));
            if (has_aggs)
            {
                out.row_aggs.push_back(std::move(batch.row_aggs[i]));
            }
        }
    }
    return out;
}

Executor::Batch Executor::exec_project(const popo::planner::ProjectNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    Batch out;
    ColumnSchema cs;
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;

    // Expand stars once to build the output schema. A null expr marks a passthrough
    // column whose input index is stored in star_columns.
    std::vector<ExprPtr> outputs;
    std::vector<size_t> star_columns;
    for (size_t i = 0; i < node.expressions.size(); ++i)
    {
        const auto & expr = node.expressions[i];
        std::string name = i < node.output_names.size() ? node.output_names[i] : "";
        if (expr->type == ExprType::Star)
        {
            const auto & star = static_cast<const popo::parser::StarExpr &>(*expr);
            size_t offset = 0;
            for (const auto & s : batch.schemas)
            {
                bool include = star.table_name.empty() || star.table_name == s.table
                    || (!s.alias.empty() && star.table_name == s.alias);
                for (const auto & col : s.columns)
                {
                    if (include)
                    {
                        cs.columns.push_back(col);
                        outputs.push_back(nullptr);
                        star_columns.push_back(offset);
                    }
                    ++offset;
                }
            }
            continue;
        }
        if (name.empty())
        {
            if (expr->type == ExprType::FunctionCall)
            {
                name = agg_display_name(static_cast<const popo::parser::FunctionCallExpr &>(*expr));
            }
            else
            {
                name = std::format("column{}", cs.columns.size());
            }
        }
        cs.columns.push_back(name);
        outputs.push_back(expr);
    }
    out.schemas.push_back(std::move(cs));

    bool has_aggs = !batch.row_aggs.empty();
    size_t star_idx = 0;
    for (size_t r = 0; r < batch.rows.size(); ++r)
    {
        const auto & row = batch.rows[r];
        env.row = &row;
        env.aggs = has_aggs ? &batch.row_aggs[r] : nullptr;
        Row projected;
        projected.reserve_values(outputs.size());
        star_idx = 0;
        for (const auto & expr : outputs)
        {
            if (expr == nullptr)
            {
                projected.append_value(row.values_[star_columns[star_idx++]]);
            }
            else
            {
                projected.append_value(eval(expr, env));
            }
        }
        out.rows.push_back(std::move(projected));
    }
    return out;
}

Executor::Batch Executor::exec_join(const popo::planner::JoinNode & node, const EvalEnv * outer)
{
    auto left = exec_batch(node.left, outer);
    auto right = exec_batch(node.right, outer);
    Batch out;
    out.schemas = left.schemas;
    out.schemas.insert(out.schemas.end(), right.schemas.begin(), right.schemas.end());

    size_t right_width = right.width();
    Row null_right;
    null_right.reserve_values(right_width);
    for (size_t i = 0; i < right_width; ++i)
    {
        null_right.append_value(Value());
    }
    Row null_left;
    null_left.reserve_values(left.width());
    for (size_t i = 0; i < left.width(); ++i)
    {
        null_left.append_value(Value());
    }

    EvalEnv env;
    env.layout = &out;
    env.outer = outer;
    auto concat = [](const Row & l, const Row & r) {
        Row row;
        row.reserve_values(l.values_.size() + r.values_.size());
        row.values_ = l.values_;
        row.values_.insert(row.values_.end(), r.values_.begin(), r.values_.end());
        return row;
    };
    auto matches = [&](const Row & l, const Row & r) {
        if (!node.condition)
            return true;
        Row joined = concat(l, r);
        env.row = &joined;
        return as_bool(eval(node.condition, env));
    };

    bool emit_unmatched_left = node.join_type == popo::parser::JoinType::Left || node.join_type == popo::parser::JoinType::Full;
    bool emit_unmatched_right = node.join_type == popo::parser::JoinType::Right || node.join_type == popo::parser::JoinType::Full;
    std::vector<bool> right_matched(right.rows.size(), false);

    for (const auto & lrow : left.rows)
    {
        bool matched = false;
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            if (matches(lrow, right.rows[j]))
            {
                matched = true;
                right_matched[j] = true;
                out.rows.push_back(concat(lrow, right.rows[j]));
            }
        }
        if (!matched && emit_unmatched_left)
        {
            out.rows.push_back(concat(lrow, null_right));
        }
    }
    if (emit_unmatched_right)
    {
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            if (!right_matched[j])
            {
                out.rows.push_back(concat(null_left, right.rows[j]));
            }
        }
    }
    return out;
}

Executor::Batch Executor::exec_agg(const popo::planner::AggNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    Batch out;
    out.schemas = batch.schemas;

    struct AggState
    {
        int64_t count = 0;
        Value result;
        bool initialized = false;
        std::set<std::string> distinct_seen;
    };
    struct Group
    {
        std::vector<AggState> states;
        const Row * representative = nullptr;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, size_t> group_index;

    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;

    for (const auto & row : batch.rows)
    {
        env.row = &row;
        std::string key;
        for (const auto & g : node.group_by)
        {
            key += value_key(eval(g, env));
            key += '\x1f';
        }
        auto it = group_index.find(key);
        if (it == group_index.end())
        {
            Group group;
            group.states.resize(node.agg_exprs.size());
            group.representative = &row;
            it = group_index.emplace(key, groups.size()).first;
            groups.push_back(std::move(group));
        }
        auto & group = groups[it->second];
        for (size_t i = 0; i < node.agg_exprs.size(); ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            auto & state = group.states[i];
            bool is_star = func.args.empty() || func.args[0]->type == ExprType::Star;
            Value val = is_star ? Value(ValueType::Int32, 1) : eval(func.args[0], env);
            if (!is_star && is_null(val))
                continue;
            if (func.is_distinct)
            {
                if (!state.distinct_seen.insert(value_key(val)).second)
                    continue;
            }
            switch (func.func)
            {
                case popo::parser::AggFuncType::Count:
                    state.count++;
                    break;
                case popo::parser::AggFuncType::Sum:
                case popo::parser::AggFuncType::Avg:
                    state.result = state.initialized ? do_arithmetic('+', state.result, val) : val;
                    state.initialized = true;
                    state.count++;
                    break;
                case popo::parser::AggFuncType::Min:
                    if (!state.initialized || sql_lt(val, state.result))
                    {
                        state.result = val;
                        state.initialized = true;
                    }
                    break;
                case popo::parser::AggFuncType::Max:
                    if (!state.initialized || sql_lt(state.result, val))
                    {
                        state.result = val;
                        state.initialized = true;
                    }
                    break;
                case popo::parser::AggFuncType::XmlAgg: {
                    std::string piece = val.datatype() == ValueType::String ? std::string(val.string_ref()) : val.to_val_string();
                    if (!state.initialized)
                    {
                        state.result = Value(ValueType::String, piece);
                        state.initialized = true;
                    }
                    else
                    {
                        state.result = Value(ValueType::String, state.result.string_ref() + piece);
                    }
                    break;
                }
            }
        }
    }

    if (groups.empty() && node.group_by.empty())
    {
        Group group;
        group.states.resize(node.agg_exprs.size());
        groups.push_back(std::move(group));
    }

    Row null_row;
    null_row.reserve_values(batch.width());
    for (size_t i = 0; i < batch.width(); ++i)
    {
        null_row.append_value(Value());
    }

    for (auto & group : groups)
    {
        std::unordered_map<const Expr *, Value> agg_values;
        for (size_t i = 0; i < node.agg_exprs.size(); ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            const auto & state = group.states[i];
            Value value;
            switch (func.func)
            {
                case popo::parser::AggFuncType::Count:
                    value = Value(ValueType::Int64, state.count);
                    break;
                case popo::parser::AggFuncType::Avg:
                    if (state.count > 0)
                        value = Value(ValueType::Float64, state.result.to_f64() / static_cast<double>(state.count));
                    break;
                default:
                    value = state.result;
                    break;
            }
            agg_values.emplace(node.agg_exprs[i].get(), value);
        }
        out.rows.push_back(group.representative ? *group.representative : null_row);
        out.row_aggs.push_back(std::move(agg_values));
    }
    return out;
}

Executor::Batch Executor::exec_sort(const popo::planner::SortNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    bool has_aggs = !batch.row_aggs.empty();
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;

    std::vector<std::vector<Value>> keys(batch.rows.size());
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        env.row = &batch.rows[i];
        env.aggs = has_aggs ? &batch.row_aggs[i] : nullptr;
        for (const auto & item : node.order_by)
        {
            keys[i].push_back(eval(item.expr, env));
        }
    }
    std::vector<size_t> order(batch.rows.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        for (size_t k = 0; k < node.order_by.size(); ++k)
        {
            const auto & av = keys[a][k];
            const auto & bv = keys[b][k];
            bool asc = node.order_by[k].ascending;
            if (is_null(av) || is_null(bv))
            {
                if (is_null(av) && is_null(bv))
                    continue;
                return asc ? is_null(av) : is_null(bv);
            }
            if (sql_eq(av, bv))
                continue;
            return asc ? sql_lt(av, bv) : sql_lt(bv, av);
        }
        return false;
    });
    Batch out;
    out.schemas = batch.schemas;
    for (size_t idx : order)
    {
        out.rows.push_back(std::move(batch.rows[idx]));
        if (has_aggs)
        {
            out.row_aggs.push_back(std::move(batch.row_aggs[idx]));
        }
    }
    return out;
}

Executor::Batch Executor::exec_limit(const popo::planner::LimitNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    Batch out;
    out.schemas = batch.schemas;
    bool has_aggs = !batch.row_aggs.empty();
    int64_t offset = std::max<int64_t>(node.offset_count, 0);
    int64_t remaining = node.limit_count < 0 ? static_cast<int64_t>(batch.rows.size()) : node.limit_count;
    for (size_t i = static_cast<size_t>(offset); i < batch.rows.size() && remaining > 0; ++i, --remaining)
    {
        out.rows.push_back(std::move(batch.rows[i]));
        if (has_aggs)
        {
            out.row_aggs.push_back(std::move(batch.row_aggs[i]));
        }
    }
    return out;
}

Executor::Batch Executor::exec_distinct(const popo::planner::DistinctNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    Batch out;
    out.schemas = batch.schemas;
    bool has_aggs = !batch.row_aggs.empty();
    std::set<std::string> seen;
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        if (seen.insert(row_key(batch.rows[i])).second)
        {
            out.rows.push_back(std::move(batch.rows[i]));
            if (has_aggs)
            {
                out.row_aggs.push_back(std::move(batch.row_aggs[i]));
            }
        }
    }
    return out;
}

Executor::Batch Executor::exec_set_op(const popo::planner::SetOpNode & node, const EvalEnv * outer)
{
    auto left = exec_batch(node.left, outer);
    auto right = exec_batch(node.right, outer);
    if (left.width() != right.width())
    {
        throw std::runtime_error("set operation branches must have the same number of columns");
    }
    Batch out;
    out.schemas = left.schemas;

    auto counts = [](const std::vector<Row> & rows)
    {
        std::unordered_map<std::string, int> n;
        for (const auto & row : rows)
        {
            n[row_key(row)]++;
        }
        return n;
    };

    if (node.kind == popo::parser::SetOpKind::Union)
    {
        out.rows = std::move(left.rows);
        out.rows.insert(out.rows.end(), std::make_move_iterator(right.rows.begin()), std::make_move_iterator(right.rows.end()));
        if (!node.is_all)
        {
            std::set<std::string> seen;
            std::vector<Row> deduped;
            for (auto & row : out.rows)
            {
                if (seen.insert(row_key(row)).second)
                {
                    deduped.push_back(std::move(row));
                }
            }
            out.rows = std::move(deduped);
        }
        return out;
    }

    auto right_count = counts(right.rows);
    std::unordered_map<std::string, int> emitted;
    for (auto & row : left.rows)
    {
        std::string key = row_key(row);
        int have = right_count[key];
        int used = emitted[key];
        bool keep = false;
        if (node.kind == popo::parser::SetOpKind::Intersect)
        {
            if (node.is_all)
            {
                if (have > 0)
                {
                    right_count[key] = have - 1;
                    keep = true;
                }
            }
            else
            {
                keep = have > 0 && used == 0;
            }
        }
        else if (node.is_all)
        {
            if (have > 0)
            {
                right_count[key] = have - 1;
            }
            else
            {
                keep = true;
            }
        }
        else
        {
            keep = have == 0 && used == 0;
        }
        emitted[key] = used + 1;
        if (keep)
        {
            out.rows.push_back(std::move(row));
        }
    }
    return out;
}

Executor::Batch Executor::exec_recursive_scan(const popo::planner::RecursiveScanNode & node)
{
    auto it = recursive_inputs_.find(node.cte_name);
    if (it == recursive_inputs_.end())
    {
        return {};
    }
    return it->second;
}

Executor::Batch Executor::exec_recursive(const popo::planner::SubqueryScanNode & node)
{
    const auto * setop = dynamic_cast<const popo::planner::SetOpNode *>(node.child.get());
    if (!setop || setop->kind != popo::parser::SetOpKind::Union || !setop->is_all)
    {
        throw std::runtime_error("WITH RECURSIVE requires a UNION ALL body");
    }
    auto anchor = exec_batch(setop->left, nullptr);
    Batch working = anchor;
    Batch delta = anchor;
    auto anchor_names = anchor.flat_names();
    for (int step = 0; step < 1000; ++step)
    {
        recursive_inputs_[node.cte_name] = delta;
        auto next = exec_batch(setop->right, nullptr);
        if (next.rows.empty())
        {
            break;
        }
        if (step == 999)
        {
            throw std::runtime_error("WITH RECURSIVE exceeded 1000 iterations");
        }
        // The recursive term's projection is unnamed. Expose the anchor column names
        // so the next iteration resolves the same CTE columns.
        if (next.flat_names().size() == anchor_names.size())
        {
            size_t cursor = 0;
            for (auto & schema : next.schemas)
            {
                for (auto & col : schema.columns)
                {
                    col = anchor_names[cursor++];
                }
            }
        }
        delta.schemas = next.schemas;
        delta.rows = next.rows;
        working.rows.insert(working.rows.end(), next.rows.begin(), next.rows.end());
    }
    recursive_inputs_.erase(node.cte_name);
    return working;
}

Executor::Batch Executor::exec_window(const popo::planner::WindowNode & node, const EvalEnv * outer)
{
    auto batch = exec_batch(node.child, outer);
    if (batch.row_aggs.size() < batch.rows.size())
    {
        batch.row_aggs.resize(batch.rows.size());
    }
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;

    auto peer = [&](const popo::parser::WindowExpr & win, size_t a, size_t b)
    {
        for (const auto & item : win.order_by)
        {
            env.row = &batch.rows[a];
            Value va = eval(item.expr, env);
            env.row = &batch.rows[b];
            Value vb = eval(item.expr, env);
            if (!sql_eq(va, vb))
            {
                return false;
            }
        }
        return true;
    };

    for (const auto & wexpr : node.windows)
    {
        const auto & win = static_cast<const popo::parser::WindowExpr &>(*wexpr);
        std::unordered_map<std::string, std::vector<size_t>> parts;
        for (size_t i = 0; i < batch.rows.size(); ++i)
        {
            env.row = &batch.rows[i];
            std::string key;
            for (const auto & part : win.partition_by)
            {
                key += value_key(eval(part, env));
                key += '\x1f';
            }
            parts[key].push_back(i);
        }
        std::string fname;
        const popo::parser::FunctionCallExpr * agg = nullptr;
        if (win.function && win.function->type == ExprType::ScalarCall)
        {
            fname = static_cast<const popo::parser::ScalarCallExpr &>(*win.function).name;
        }
        else if (win.function && win.function->type == ExprType::FunctionCall)
        {
            agg = &static_cast<const popo::parser::FunctionCallExpr &>(*win.function);
        }
        for (auto & [_, idxs] : parts)
        {
            std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b)
            {
                for (const auto & item : win.order_by)
                {
                    env.row = &batch.rows[a];
                    Value va = eval(item.expr, env);
                    env.row = &batch.rows[b];
                    Value vb = eval(item.expr, env);
                    if (sql_lt(va, vb))
                        return item.ascending;
                    if (sql_lt(vb, va))
                        return !item.ascending;
                }
                return false;
            });
            if (fname == "ROW_NUMBER" || fname == "RANK" || fname == "DENSE_RANK")
            {
                int dense = 0;
                int64_t rank_value = 0;
                for (size_t i = 0; i < idxs.size(); ++i)
                {
                    bool new_peer = i == 0 || !peer(win, idxs[i], idxs[i - 1]);
                    if (new_peer)
                    {
                        ++dense;
                        rank_value = static_cast<int64_t>(i + 1);
                    }
                    int64_t value = fname == "ROW_NUMBER" ? static_cast<int64_t>(i + 1) : fname == "RANK" ? rank_value : dense;
                    batch.row_aggs[idxs[i]][wexpr.get()] = Value(ValueType::Int64, value);
                }
            }
            else if (agg)
            {
                auto accumulate = [&](size_t index, Value & acc, bool & init, int64_t & count)
                {
                    env.row = &batch.rows[index];
                    bool is_star = agg->args.empty() || agg->args[0]->type == ExprType::Star;
                    Value val = is_star ? Value(ValueType::Int32, 1) : eval(agg->args[0], env);
                    if (!is_star && is_null(val))
                        return;
                    if (agg->func == popo::parser::AggFuncType::Count)
                    {
                        ++count;
                        return;
                    }
                    if (agg->func == popo::parser::AggFuncType::Avg || agg->func == popo::parser::AggFuncType::Sum)
                    {
                        acc = init ? do_arithmetic('+', acc, val) : val;
                        init = true;
                        ++count;
                        return;
                    }
                    if (agg->func == popo::parser::AggFuncType::Min)
                    {
                        if (!init || sql_lt(val, acc))
                            acc = val;
                        init = true;
                        return;
                    }
                    if (agg->func == popo::parser::AggFuncType::Max)
                    {
                        if (!init || sql_lt(acc, val))
                            acc = val;
                        init = true;
                    }
                };
                auto finish = [&](const Value & acc, bool init, int64_t count)
                {
                    if (agg->func == popo::parser::AggFuncType::Count)
                        return Value(ValueType::Int64, count);
                    if (agg->func == popo::parser::AggFuncType::Avg)
                    {
                        if (count == 0)
                            return Value();
                        return Value(ValueType::Float64, acc.to_f64() / static_cast<double>(count));
                    }
                    return init ? acc : Value();
                };
                if (win.order_by.empty())
                {
                    Value acc;
                    bool init = false;
                    int64_t count = 0;
                    for (size_t index : idxs)
                    {
                        accumulate(index, acc, init, count);
                    }
                    Value value = finish(acc, init, count);
                    for (size_t index : idxs)
                    {
                        batch.row_aggs[index][wexpr.get()] = value;
                    }
                }
                else
                {
                    Value acc;
                    bool init = false;
                    int64_t count = 0;
                    for (size_t index : idxs)
                    {
                        accumulate(index, acc, init, count);
                        batch.row_aggs[index][wexpr.get()] = finish(acc, init, count);
                    }
                }
            }
        }
    }
    return batch;
}

Value Executor::resolve_column(const std::string & table, const std::string & column, const EvalEnv & env) const
{
    const EvalEnv * scope = &env;
    while (scope)
    {
        if (scope->layout && scope->row)
        {
            size_t offset = 0;
            for (const auto & s : scope->layout->schemas)
            {
                bool table_match = table.empty() || table == s.table || (!s.alias.empty() && table == s.alias);
                if (table_match)
                {
                    for (size_t i = 0; i < s.columns.size(); ++i)
                    {
                        if (s.columns[i] == column)
                        {
                            size_t idx = offset + i;
                            if (idx < scope->row->values_.size())
                            {
                                return scope->row->values_[idx];
                            }
                            return Value();
                        }
                    }
                }
                offset += s.columns.size();
            }
        }
        scope = scope->outer;
    }
    throw std::runtime_error(std::format("Column '{}' not found", table.empty() ? column : table + "." + column));
}

const popo::planner::PlanNodePtr &
Executor::subquery_plan(const popo::parser::Expr & expr, const popo::parser::StmtPtr & query)
{
    auto it = subquery_plans_.find(&expr);
    if (it == subquery_plans_.end())
    {
        auto planned = planner_.plan(query);
        if (!planned)
        {
            throw std::runtime_error(planned.error().error_message);
        }
        it = subquery_plans_.emplace(&expr, *planned).first;
    }
    return it->second;
}

Value Executor::eval_subquery(const popo::parser::Expr & expr, const EvalEnv & env)
{
    switch (expr.type)
    {
        case ExprType::Subquery: {
            const auto & sub = static_cast<const popo::parser::SubqueryExpr &>(expr);
            auto batch = exec_batch(subquery_plan(expr, sub.query), &env);
            if (batch.rows.empty())
                return Value();
            if (batch.rows.size() > 1)
                throw std::runtime_error("scalar subquery returned more than one row");
            if (batch.rows[0].values_.empty())
                throw std::runtime_error("scalar subquery must return one column");
            return batch.rows[0].values_[0];
        }
        case ExprType::InSubquery: {
            const auto & in = static_cast<const popo::parser::InSubqueryExpr &>(expr);
            Value operand = eval(in.operand, env);
            auto batch = exec_batch(subquery_plan(expr, in.query), &env);
            bool found = false;
            if (!is_null(operand))
            {
                for (const auto & row : batch.rows)
                {
                    if (!row.values_.empty() && sql_eq(operand, row.values_[0]))
                    {
                        found = true;
                        break;
                    }
                }
            }
            return make_bool(in.negated ? !found : found);
        }
        case ExprType::Exists: {
            const auto & ex = static_cast<const popo::parser::ExistsExpr &>(expr);
            auto batch = exec_batch(subquery_plan(expr, ex.query), &env);
            return make_bool(!batch.rows.empty());
        }
        default:
            throw std::runtime_error("not a subquery expression");
    }
}

std::string trimmed_text(const std::string & text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

int64_t cast_i64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return to_i64(v);
    std::string text = trimmed_text(v.string_ref());
    if (text.empty())
        throw std::runtime_error("CAST from empty string");
    try
    {
        size_t used = 0;
        long long n = std::stoll(text, &used);
        if (used == text.size())
            return static_cast<int64_t>(n);
        double d = std::stod(text, &used);
        if (used != text.size())
            throw std::runtime_error("CAST could not parse '" + text + "'");
        return static_cast<int64_t>(d);
    }
    catch (const std::invalid_argument &)
    {
        throw std::runtime_error("CAST could not parse '" + text + "'");
    }
    catch (const std::out_of_range &)
    {
        throw std::runtime_error("CAST overflow");
    }
}

double cast_f64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return v.to_f64();
    std::string text = trimmed_text(v.string_ref());
    try
    {
        size_t used = 0;
        double d = std::stod(text, &used);
        if (used != text.size())
            throw std::runtime_error("CAST could not parse '" + text + "'");
        return d;
    }
    catch (const std::invalid_argument &)
    {
        throw std::runtime_error("CAST could not parse '" + text + "'");
    }
    catch (const std::out_of_range &)
    {
        throw std::runtime_error("CAST overflow");
    }
}

Value cast_to(const Value & v, ValueType target)
{
    if (is_null(v))
        return Value();
    switch (target)
    {
        case ValueType::Bool:
            return Value(ValueType::Bool, as_bool(v));
        case ValueType::Int8:
            return Value(ValueType::Int8, static_cast<int8_t>(cast_i64(v)));
        case ValueType::Int16:
            return Value(ValueType::Int16, static_cast<int16_t>(cast_i64(v)));
        case ValueType::Int32:
            return Value(ValueType::Int32, static_cast<int32_t>(cast_i64(v)));
        case ValueType::Int64:
            return Value(ValueType::Int64, cast_i64(v));
        case ValueType::Float32:
            return Value(ValueType::Float32, static_cast<float>(cast_f64(v)));
        case ValueType::Float64:
            return Value(ValueType::Float64, cast_f64(v));
        case ValueType::String:
            return Value(ValueType::String, v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string());
        default:
            throw std::runtime_error("unsupported CAST target");
    }
}

std::string xml_text(const Value & v)
{
    std::string raw = v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string();
    std::string out;
    for (char c : raw)
    {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out.push_back(c);
    }
    return out;
}

bool sql_cmp(popo::parser::BinaryOpType op, const Value & l, const Value & r)
{
    switch (op)
    {
        case popo::parser::BinaryOpType::Eq:
            return sql_eq(l, r);
        case popo::parser::BinaryOpType::Ne:
            return comparable(l, r) && !sql_eq(l, r);
        case popo::parser::BinaryOpType::Lt:
            return sql_lt(l, r);
        case popo::parser::BinaryOpType::Gt:
            return sql_lt(r, l);
        case popo::parser::BinaryOpType::Le:
            return comparable(l, r) && !sql_lt(r, l);
        case popo::parser::BinaryOpType::Ge:
            return comparable(l, r) && !sql_lt(l, r);
        default:
            return false;
    }
}

Value Executor::eval_quantified(const popo::parser::QuantifiedExpr & expr, const EvalEnv & env)
{
    Value operand = eval(expr.operand, env);
    auto batch = exec_batch(subquery_plan(expr, expr.query), &env);
    if (batch.rows.empty())
    {
        return make_bool(expr.all);
    }
    if (expr.all)
    {
        for (const auto & row : batch.rows)
        {
            if (row.values_.empty() || !sql_cmp(expr.op, operand, row.values_[0]))
                return make_bool(false);
        }
        return make_bool(true);
    }
    for (const auto & row : batch.rows)
    {
        if (!row.values_.empty() && sql_cmp(expr.op, operand, row.values_[0]))
            return make_bool(true);
    }
    return make_bool(false);
}

Value Executor::eval_scalar(const popo::parser::ScalarCallExpr & expr, const EvalEnv & env)
{
    std::vector<Value> args;
    args.reserve(expr.args.size());
    for (const auto & arg : expr.args)
    {
        args.push_back(eval(arg, env));
    }
    const std::string & name = expr.name;
    auto text_of = [](const Value & v)
    { return v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string(); };
    if (name == "COALESCE")
    {
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                return arg;
        }
        return Value();
    }
    if (name == "NULLIF")
    {
        if (args.size() != 2)
            throw std::runtime_error("NULLIF expects 2 arguments");
        return sql_eq(args[0], args[1]) ? Value() : args[0];
    }
    if (name == "UPPER" || name == "LOWER")
    {
        if (args.size() != 1)
            throw std::runtime_error(name + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        for (char & c : text)
        {
            c = static_cast<char>(name == "UPPER" ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
        }
        return Value(ValueType::String, text);
    }
    if (name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH" || name == "LENGTH")
    {
        if (args.size() != 1)
            throw std::runtime_error(name + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        return Value(ValueType::Int64, static_cast<int64_t>(text_of(args[0]).size()));
    }
    if (name == "SUBSTRING" || name == "SUBSTR")
    {
        if (args.size() < 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        std::string text = text_of(args[0]);
        int64_t start = to_i64(args[1]);
        if (start < 1)
            start = 1;
        size_t offset = static_cast<size_t>(start - 1);
        if (offset >= text.size())
            return Value(ValueType::String, std::string());
        size_t len = text.size() - offset;
        if (args.size() >= 3 && !is_null(args[2]))
        {
            int64_t asked = to_i64(args[2]);
            if (asked < 0)
                asked = 0;
            len = std::min(len, static_cast<size_t>(asked));
        }
        return Value(ValueType::String, text.substr(offset, len));
    }
    if (name == "TRIM")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        size_t begin = 0;
        while (begin < text.size() && text[begin] == ' ')
            ++begin;
        size_t end = text.size();
        while (end > begin && text[end - 1] == ' ')
            --end;
        return Value(ValueType::String, text.substr(begin, end - begin));
    }
    if (name == "ABS")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        if (is_floating(args[0]))
            return Value(ValueType::Float64, std::fabs(args[0].to_f64()));
        int64_t n = to_i64(args[0]);
        return Value(ValueType::Int64, n < 0 ? -n : n);
    }
    if (name == "CONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (is_null(arg))
                return Value();
            text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLCOMMENT")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::String, "<!--" + text_of(args[0]) + "-->");
    }
    if (name == "XMLCONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLELEMENT")
    {
        if (args.empty())
            throw std::runtime_error("XMLELEMENT requires a name");
        std::string tag = text_of(args[0]);
        std::string body = args.size() > 1 && !is_null(args[1]) ? xml_text(args[1]) : "";
        return Value(ValueType::String, "<" + tag + ">" + body + "</" + tag + ">");
    }
    if (name == "XMLFOREST")
    {
        std::string text;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            if (is_null(args[i + 1]))
                continue;
            std::string tag = text_of(args[i]);
            text += "<" + tag + ">" + xml_text(args[i + 1]) + "</" + tag + ">";
        }
        return Value(ValueType::String, text);
    }
    throw std::runtime_error("unknown function " + name);
}

Value Executor::eval(const ExprPtr & expr, const EvalEnv & env)
{
    if (!expr)
        return Value();
    switch (expr->type)
    {
        case ExprType::Literal:
            return static_cast<const popo::parser::LiteralExpr &>(*expr).value;
        case ExprType::ColumnRef: {
            const auto & col = static_cast<const popo::parser::ColumnRefExpr &>(*expr);
            return resolve_column(col.table_name, col.column_name, env);
        }
        case ExprType::Alias:
            return eval(static_cast<const popo::parser::AliasExpr &>(*expr).expr, env);
        case ExprType::BinaryOp: {
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            Value l = eval(bin.left, env);
            Value r = eval(bin.right, env);
            switch (bin.op)
            {
                case popo::parser::BinaryOpType::Add:
                    return do_arithmetic('+', l, r);
                case popo::parser::BinaryOpType::Sub:
                    return do_arithmetic('-', l, r);
                case popo::parser::BinaryOpType::Mul:
                    return do_arithmetic('*', l, r);
                case popo::parser::BinaryOpType::Div:
                    return do_arithmetic('/', l, r);
                case popo::parser::BinaryOpType::Eq:
                    return make_bool(sql_eq(l, r));
                case popo::parser::BinaryOpType::Ne:
                    return make_bool(comparable(l, r) && !(l == r));
                case popo::parser::BinaryOpType::Lt:
                    return make_bool(sql_lt(l, r));
                case popo::parser::BinaryOpType::Le:
                    return make_bool(comparable(l, r) && !(r < l));
                case popo::parser::BinaryOpType::Gt:
                    return make_bool(sql_lt(r, l));
                case popo::parser::BinaryOpType::Ge:
                    return make_bool(comparable(l, r) && !(l < r));
                case popo::parser::BinaryOpType::And:
                    return make_bool(as_bool(l) && as_bool(r));
                case popo::parser::BinaryOpType::Or:
                    return make_bool(as_bool(l) || as_bool(r));
                case popo::parser::BinaryOpType::Concat: {
                    if (is_null(l) || is_null(r))
                        return Value();
                    auto text = [](const Value & v)
                    { return v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string(); };
                    return Value(ValueType::String, text(l) + text(r));
                }
            }
            throw std::runtime_error("unknown binary operator");
        }
        case ExprType::UnaryOp: {
            const auto & un = static_cast<const popo::parser::UnaryOpExpr &>(*expr);
            Value v = eval(un.operand, env);
            if (un.op == popo::parser::UnaryOpType::Not)
                return make_bool(!as_bool(v));
            if (is_null(v))
                return Value();
            if (is_floating(v))
                return Value(ValueType::Float64, -v.to_f64());
            return Value(ValueType::Int64, -static_cast<int64_t>(v.to_f64()));
        }
        case ExprType::FunctionCall: {
            const Expr * key = expr.get();
            const EvalEnv * scope = &env;
            while (scope)
            {
                if (scope->aggs)
                {
                    auto it = scope->aggs->find(key);
                    if (it != scope->aggs->end())
                        return it->second;
                }
                scope = scope->outer;
            }
            throw std::runtime_error("aggregate function not allowed in this context");
        }
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            Value operand = eval(in.operand, env);
            bool found = false;
            if (!is_null(operand))
            {
                for (const auto & item : in.list)
                {
                    if (sql_eq(operand, eval(item, env)))
                    {
                        found = true;
                        break;
                    }
                }
            }
            return make_bool(in.negated ? !found : found);
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            Value v = eval(b.operand, env);
            Value lo = eval(b.low, env);
            Value hi = eval(b.high, env);
            bool in_range = comparable(v, lo) && comparable(v, hi) && !(v < lo) && !(hi < v);
            return make_bool(b.negated ? !in_range : in_range);
        }
        case ExprType::Like: {
            const auto & like = static_cast<const popo::parser::LikeExpr &>(*expr);
            Value v = eval(like.operand, env);
            Value p = eval(like.pattern, env);
            if (is_null(v) || is_null(p))
                return make_bool(false);
            bool matched = like_match(v.to_val_string(), p.to_val_string());
            return make_bool(like.negated ? !matched : matched);
        }
        case ExprType::IsNull: {
            const auto & isnull = static_cast<const popo::parser::IsNullExpr &>(*expr);
            bool null = is_null(eval(isnull.operand, env));
            return make_bool(isnull.negated ? !null : null);
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            Value operand = c.operand ? eval(c.operand, env) : Value();
            for (const auto & [cond, result] : c.whens)
            {
                Value cv = eval(cond, env);
                bool hit = c.operand ? sql_eq(operand, cv) : as_bool(cv);
                if (hit)
                    return eval(result, env);
            }
            return c.else_expr ? eval(c.else_expr, env) : Value();
        }
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
            return eval_subquery(*expr, env);
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            Value l = eval(d.left, env);
            Value r = eval(d.right, env);
            bool same = (is_null(l) && is_null(r)) || sql_eq(l, r);
            return make_bool(d.negated ? same : !same);
        }
        case ExprType::Quantified:
            return eval_quantified(static_cast<const popo::parser::QuantifiedExpr &>(*expr), env);
        case ExprType::ScalarCall:
            return eval_scalar(static_cast<const popo::parser::ScalarCallExpr &>(*expr), env);
        case ExprType::Cast: {
            const auto & cast_expr = static_cast<const popo::parser::CastExpr &>(*expr);
            return cast_to(eval(cast_expr.operand, env), cast_expr.target);
        }
        case ExprType::Window: {
            if (!env.aggs)
                throw std::runtime_error("window function is not available here");
            auto found = env.aggs->find(expr.get());
            if (found == env.aggs->end())
                throw std::runtime_error("window function was not computed");
            return found->second;
        }
        case ExprType::Star:
            throw std::runtime_error("'*' is only valid in a select list or COUNT(*)");
    }
    throw std::runtime_error("unknown expression type");
}

void Executor::exec_insert(const popo::planner::InsertNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", node.table_name));
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
    {
        throw std::runtime_error(std::format("Table '{}' has no storage", node.table_name));
    }
    const auto & schema = *qr;
    std::vector<int32_t> positions(schema->columns_.size(), -1);
    if (node.columns.empty())
    {
        for (size_t i = 0; i < positions.size(); ++i)
        {
            positions[i] = static_cast<int32_t>(i);
        }
    }
    else
    {
        for (size_t i = 0; i < node.columns.size(); ++i)
        {
            bool found = false;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (schema->columns_[j].column_name_ == node.columns[i])
                {
                    positions[j] = static_cast<int32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                throw std::runtime_error(std::format("Column '{}' not found", node.columns[i]));
            }
        }
    }

    EvalEnv env;
    for (const auto & values : node.values)
    {
        if (values.size() > (node.columns.empty() ? schema->columns_.size() : node.columns.size()))
        {
            throw std::runtime_error("INSERT has more values than columns");
        }
        std::vector<Value> evaluated;
        evaluated.reserve(values.size());
        for (const auto & e : values)
        {
            evaluated.push_back(eval(e, env));
        }
        Row row;
        row.reserve_values(schema->columns_.size());
        for (size_t j = 0; j < schema->columns_.size(); ++j)
        {
            int32_t pos = positions[j];
            if (pos >= 0 && static_cast<size_t>(pos) < evaluated.size())
            {
                row.append_value(evaluated[static_cast<size_t>(pos)]);
            }
            else if (schema->columns_[j].default_value_.has_value())
            {
                row.append_value(*schema->columns_[j].default_value_);
            }
            else
            {
                row.append_value(Value());
            }
        }
        auto tuple = popo::storage::Tuple::form(heap->schema(), row);
        auto rid = heap->insert_row(txn_, tuple);
        if (!rid.is_valid())
        {
            throw std::runtime_error("INSERT failed");
        }
        result_.rows_affected++;
    }
}

void Executor::exec_update(const popo::planner::UpdateNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", node.table_name));
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return;
    const auto & schema = *qr;

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : schema->columns_)
    {
        cs.columns.push_back(col.column_name_);
    }
    layout.schemas.push_back(std::move(cs));

    std::vector<std::pair<popo::storage::RowId, Row>> targets;
    {
        popo::storage::TableIterator iter(heap, heap->schema());
        EvalEnv env;
        env.layout = &layout;
        for (iter.rewind(); iter.valid(); iter.next())
        {
            Row row = iter.current_row();
            env.row = &row;
            if (!node.where_clause || as_bool(eval(node.where_clause, env)))
            {
                targets.emplace_back(iter.current_row_id(), std::move(row));
            }
        }
    }
    for (auto & [rid, row] : targets)
    {
        EvalEnv env;
        env.layout = &layout;
        env.row = &row;
        Row updated = row;
        for (const auto & assign : node.assignments)
        {
            int32_t idx = -1;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (schema->columns_[j].column_name_ == assign.column_name)
                {
                    idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found", assign.column_name));
            }
            updated.values_[static_cast<size_t>(idx)] = eval(assign.value, env);
        }
        auto tuple = popo::storage::Tuple::form(heap->schema(), updated);
        heap->update_row(txn_, rid, tuple);
        result_.rows_affected++;
    }
}

void Executor::exec_delete(const popo::planner::DeleteNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", node.table_name));
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return;

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : (*qr)->columns_)
    {
        cs.columns.push_back(col.column_name_);
    }
    layout.schemas.push_back(std::move(cs));

    std::vector<popo::storage::RowId> targets;
    {
        popo::storage::TableIterator iter(heap, heap->schema());
        EvalEnv env;
        env.layout = &layout;
        for (iter.rewind(); iter.valid(); iter.next())
        {
            Row row = iter.current_row();
            env.row = &row;
            if (!node.where_clause || as_bool(eval(node.where_clause, env)))
            {
                targets.push_back(iter.current_row_id());
            }
        }
    }
    for (auto rid : targets)
    {
        heap->delete_row(txn_, rid);
        result_.rows_affected++;
    }
}

void Executor::exec_alter_table(const popo::planner::AlterTableNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", node.table_name));
    }
    const auto old_schema = **qr;
    auto * heap = engine_.get_table_heap(node.table_name);

    std::vector<Row> rows;
    if (heap)
    {
        popo::storage::TableIterator iter(heap, heap->schema());
        for (iter.rewind(); iter.valid(); iter.next())
        {
            rows.push_back(iter.current_row());
        }
    }

    auto columns = old_schema.columns_;
    std::string new_name = node.table_name;

    switch (node.action)
    {
        case popo::parser::AlterAction::AddColumn: {
            for (const auto & col : columns)
            {
                if (col.column_name_ == node.column.name)
                {
                    throw std::runtime_error(std::format("Column '{}' already exists", node.column.name));
                }
            }
            popo::catalog::Column col{};
            col.column_name_ = node.column.name;
            col.data_type_ = node.column.data_type;
            col.type_len_ = node.column.type_len;
            col.is_nullable_ = node.column.is_nullable;
            col.default_value_ = node.column.default_value;
            columns.push_back(col);
            Value fill = node.column.default_value.value_or(Value());
            for (auto & row : rows)
            {
                row.append_value(fill);
            }
            break;
        }
        case popo::parser::AlterAction::DropColumn: {
            int32_t idx = -1;
            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (columns[i].column_name_ == node.target_name)
                {
                    idx = static_cast<int32_t>(i);
                    break;
                }
            }
            if (idx < 0)
            {
                throw std::runtime_error(std::format("Column '{}' not found", node.target_name));
            }
            if (idx == old_schema.primary_key_col_idx_)
            {
                throw std::runtime_error("cannot drop the primary key column");
            }
            if (columns.size() == 1)
            {
                throw std::runtime_error("cannot drop the last column");
            }
            columns.erase(columns.begin() + idx);
            for (auto & row : rows)
            {
                row.values_.erase(row.values_.begin() + idx);
            }
            break;
        }
        case popo::parser::AlterAction::RenameTable: {
            auto existing = schema_mgr_.query_table(node.target_name);
            if (existing && *existing)
            {
                throw std::runtime_error(std::format("Table '{}' already exists", node.target_name));
            }
            new_name = node.target_name;
            break;
        }
    }

    rebuild_table(node.table_name, columns, rows, new_name);
    result_.message = "Table altered";
}

void Executor::rebuild_table(
    const std::string & table_name, const std::vector<popo::catalog::Column> & new_columns, const std::vector<Row> & rows,
    const std::string & new_name)
{
    auto qr = schema_mgr_.query_table(table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", table_name));
    }
    const auto old_schema = **qr;

    if (new_name != table_name)
    {
        std::vector<popo::catalog::Column> renamed = new_columns;
        auto created = schema_mgr_.create_table(new_name, renamed);
        if (!created)
        {
            throw std::runtime_error("failed to create renamed table: " + new_name);
        }
        auto del = schema_mgr_.delete_table(table_name);
        if (!del)
        {
            throw std::runtime_error(del.error().error_message);
        }
    }
    else
    {
        auto modified = schema_mgr_.modify_table(table_name, new_columns);
        if (!modified)
        {
            throw std::runtime_error(modified.error().error_message);
        }
    }

    engine_.drop_table(table_name);
    auto new_qr = schema_mgr_.query_table(new_name);
    if (!new_qr || !*new_qr)
    {
        throw std::runtime_error("schema missing after ALTER TABLE");
    }
    engine_.create_table(new_name, **new_qr);
    auto * heap = engine_.get_table_heap(new_name);
    if (!heap)
    {
        throw std::runtime_error("storage missing after ALTER TABLE");
    }

    // Recreate secondary indexes whose columns survived, remapping column positions
    for (const auto & index : old_schema.indexes_)
    {
        std::vector<int32_t> mapped;
        bool valid = true;
        for (int32_t old_idx : index.index_cols_)
        {
            int32_t new_idx = -1;
            for (size_t j = 0; j < new_columns.size(); ++j)
            {
                if (old_idx >= 0 && static_cast<size_t>(old_idx) < old_schema.columns_.size()
                    && new_columns[j].column_name_ == old_schema.columns_[static_cast<size_t>(old_idx)].column_name_)
                {
                    new_idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (new_idx < 0)
            {
                valid = false;
                break;
            }
            mapped.push_back(new_idx);
        }
        if (new_name == table_name)
        {
            auto del = schema_mgr_.delete_index(table_name, index.index_name_);
            if (!del)
            {
                throw std::runtime_error(del.error().error_message);
            }
        }
        if (!valid || mapped.size() != 1)
            continue;
        auto created = schema_mgr_.create_index(new_name, index.index_name_, mapped, index.is_unique_);
        if (!created)
        {
            throw std::runtime_error(created.error().error_message);
        }
        engine_.create_index(new_name, index.index_name_, mapped[0], index.is_unique_);
    }

    const auto & schema = heap->schema();
    for (const auto & row : rows)
    {
        auto tuple = popo::storage::Tuple::form(schema, row);
        auto rid = heap->insert_row(txn_, tuple);
        if (!rid.is_valid())
        {
            throw std::runtime_error("ALTER TABLE: failed to rewrite row");
        }
    }
}

void Executor::exec_create_view(const popo::planner::CreateViewNode & node)
{
    auto created = schema_mgr_.create_view(node.name, node.query);
    if (!created)
    {
        throw std::runtime_error(created.error().error_message);
    }
    result_.message = "View created";
}

void Executor::exec_drop_view(const popo::planner::DropViewNode & node)
{
    auto dropped = schema_mgr_.drop_view(node.name);
    if (!dropped)
    {
        throw std::runtime_error(dropped.error().error_message);
    }
    result_.message = "View dropped";
}

} // namespace popo::executor