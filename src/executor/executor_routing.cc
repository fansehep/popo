#include "executor/executor.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>

#include "executor/executor_internal.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::eq_ci;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;

namespace
{

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
        if (sel.distinct || sel.offset.has_value() || !sel.set_ops.empty() || !sel.with_clause.empty() || !sel.from
            || !sel.group_by.empty())
            return true;
        bool has_any_agg = sel.having != nullptr;
        for (const auto & e : sel.select_list)
        {
            if (e && e->type == ExprType::FunctionCall)
                has_any_agg = true;
        }
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
                return true;
            if (ref->type == popo::parser::TableRefType::Simple)
            {
                const auto & s = static_cast<const popo::parser::SimpleTableRef &>(*ref);
                return !s.column_aliases.empty();
            }
            if (ref->type == popo::parser::TableRefType::Join)
            {
                const auto & join = static_cast<const popo::parser::JoinTableRef &>(*ref);
                if (join.join_type != popo::parser::JoinType::Inner || has_any_agg || !sel.order_by.empty() || sel.limit.has_value())
                    return true;
                if (!join.left || !join.right || join.left->type != popo::parser::TableRefType::Simple
                    || join.right->type != popo::parser::TableRefType::Simple)
                    return true;
                const auto & ls = static_cast<const popo::parser::SimpleTableRef &>(*join.left);
                const auto & rs = static_cast<const popo::parser::SimpleTableRef &>(*join.right);
                if (eq_ci(ls.table_name, rs.table_name))
                    return true;
                return check_ref(join.left) || check_ref(join.right) || expr_has_new_syntax(join.condition);
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
            for (const auto & e : row)
                if (expr_has_new_syntax(e))
                    return true;
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Update)
    {
        const auto & upd = static_cast<const popo::parser::UpdateStatement &>(*stmt);
        if (expr_has_new_syntax(upd.where))
            return true;
        for (const auto & assign : upd.assignments)
            if (expr_has_new_syntax(assign.value))
                return true;
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
        case ExprType::Interval:
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
            if (func.is_distinct || func.func == popo::parser::AggFuncType::XmlAgg)
                return true;
            for (const auto & arg : func.args)
                if (expr_has_new_syntax(arg))
                    return true;
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

Executor::Executor(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine)
    : Executor(schema_mgr, engine, default_executor_options())
{
}

Executor::Executor(
    popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine,
    Options options)
    : schema_mgr_(schema_mgr), engine_(engine), planner_(schema_mgr), options_(std::move(options))
{
}

popo::base::Result<popo::bytecode::ExecuteSuccess>
Executor::execute(const popo::planner::PlanNodePtr & plan, popo::txn::Transaction * outer_txn)
{
    result_ = popo::bytecode::ExecuteSuccess();
    subquery_plans_.clear();
    uncorrelated_subqueries_.clear();
    correlated_subqueries_.clear();
    decorrelated_exists_.clear();
    decorrelated_scalar_aggs_.clear();
    compiled_likes_.clear();
    materialized_.clear();
    stream_materialized_.clear();
    recursive_inputs_.clear();
    spill_stats_ = {};
    memory_ = std::make_unique<MemoryTracker>(std::max<size_t>(options_.memory_limit_bytes, 4096));
    temp_ = std::make_unique<TempFileManager>(options_.temp_directory);
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    own_txn_ = outer_txn == nullptr;
    txn_ = own_txn_ ? engine_.begin_txn() : outer_txn;
    auto fail = [&](popo::base::Error error) -> popo::base::Result<popo::bytecode::ExecuteSuccess>
    {
        if (txn_ && own_txn_)
            engine_.abort_txn(txn_);
        txn_ = nullptr;
        if (memory_)
            spill_stats_.peak_memory_bytes = memory_->peak();
        if (temp_)
        {
            spill_stats_.files_created = temp_->files_created();
            spill_stats_.bytes_written = temp_->bytes_written();
        }
        temp_.reset();
        memory_.reset();
        return std::unexpected(std::move(error));
    };
    if (!plan)
    {
        return fail(popo::base::Error(
            static_cast<int32_t>(popo::base::InvalidArg), "executor received a null plan"));
    }
    engine_.mvcc()->begin_statement(txn_);
    allow_decorrelation_ = false;
    popo::base::Result<void> executed;
    switch (plan->type)
    {
        case popo::planner::PlanNodeType::Insert:
            executed = exec_insert(static_cast<const popo::planner::InsertNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::Update:
            executed = exec_update(static_cast<const popo::planner::UpdateNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::Delete:
            executed = exec_delete(static_cast<const popo::planner::DeleteNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::AlterTable:
            executed = exec_alter_table(static_cast<const popo::planner::AlterTableNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::CreateView:
            executed = exec_create_view(static_cast<const popo::planner::CreateViewNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::DropView:
            executed = exec_drop_view(static_cast<const popo::planner::DropViewNode &>(*plan));
            break;
        default: {
            allow_decorrelation_ = true;
            auto opt_plan = push_projections(optimize_plan(plan, false));
            auto streamed = stream_rows(
                opt_plan, nullptr,
                [&](StreamItem && item, const Batch &) -> popo::base::Result<bool>
                {
                    result_.rows.push_back(std::make_shared<Row>(std::move(item.row)));
                    return true;
                });
            if (!streamed)
                return fail(streamed.error());
            result_.columns = streamed->flat_names();
            break;
        }
    }
    if (!executed)
        return fail(executed.error());
    if (own_txn_ && txn_)
    {
        auto * open = txn_;
        txn_ = nullptr;
        auto committed = engine_.commit_txn(open);
        if (!committed)
            return std::unexpected(committed.error());
    }
    txn_ = nullptr;

    if (!result_.rows.empty())
        result_.message = std::to_string(result_.rows.size()) + " rows in set";
    else if (result_.rows_affected > 0)
        result_.message = std::to_string(result_.rows_affected) + " rows affected";
    else if (result_.message.empty())
        result_.message = "OK";
    spill_stats_.peak_memory_bytes = memory_ ? memory_->peak() : 0;
    spill_stats_.files_created = temp_ ? temp_->files_created() : 0;
    spill_stats_.bytes_written = temp_ ? temp_->bytes_written() : 0;
    temp_.reset();
    memory_.reset();
    return result_;
}

popo::base::Result<Batch>
Executor::exec_batch(const popo::planner::PlanNodePtr & node, const EvalEnv * outer)
{
    if (!node)
        return {};
    switch (node->type)
    {
        case popo::planner::PlanNodeType::Scan:
            return exec_scan(static_cast<const popo::planner::ScanNode &>(*node));
        case popo::planner::PlanNodeType::SubqueryScan:
            return exec_subquery_scan(static_cast<const popo::planner::SubqueryScanNode &>(*node), outer);
        case popo::planner::PlanNodeType::Dual: {
            Batch batch;
            batch.rows.emplace_back();
            return batch;
        }
        case popo::planner::PlanNodeType::Filter:
            return exec_filter(static_cast<const popo::planner::FilterNode &>(*node), outer);
        case popo::planner::PlanNodeType::Project:
            return exec_project(static_cast<const popo::planner::ProjectNode &>(*node), outer);
        case popo::planner::PlanNodeType::Join: {
            Batch out;
            auto layout = stream_join(
                static_cast<const popo::planner::JoinNode &>(*node), outer,
                [&](StreamItem && item, const Batch & l) -> popo::base::Result<bool>
                {
                    if (out.schemas.empty())
                        out.schemas = l.schemas;
                    out.rows.push_back(std::move(item.row));
                    return true;
                });
            if (!layout)
                return std::unexpected(layout.error());
            if (out.schemas.empty())
                out.schemas = layout->schemas;
            return out;
        }
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
            return executor_error("unsupported plan node in executor", popo::base::Errno::InternalErr);
    }
}

} // namespace popo::executor
