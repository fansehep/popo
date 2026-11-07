#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "base/error.h"
#include "bytecode/vm.h"
#include "catalog/schema_mgr.h"
#include "parser/ast.h"
#include "planner/planner.h"
#include "storage/storage_engine.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

// True when the statement uses syntax the bytecode compiler does not support
// (subqueries, CTE, UNION, DISTINCT, OFFSET, CASE, IN/BETWEEN/LIKE/IS NULL,
// aliases, derived tables, FROM-less SELECT, ALTER TABLE).
bool needs_generic_executor(const popo::parser::StmtPtr & stmt);

// True when planning expanded a view, CTE, or other node the bytecode VM does not run.
bool plan_needs_generic_executor(const popo::planner::PlanNodePtr & plan);

// Row-at-a-time interpreter for plan trees. Handles the full SQL surface;
// simple statements keep using the faster bytecode VM.
class Executor
{
public:
    Executor(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine);

    // outer_txn == nullptr: this call begins and commits one transaction.
    popo::base::Result<popo::bytecode::ExecuteSuccess>
    execute(const popo::planner::PlanNodePtr & plan, popo::txn::Transaction * outer_txn = nullptr);

private:
    struct ColumnSchema
    {
        std::string table;
        std::string alias;
        std::vector<std::string> columns;
    };

    struct Batch
    {
        std::vector<ColumnSchema> schemas;
        std::vector<popo::type::Row> rows;
        // Per-row aggregate results (aligned with rows) when produced by an Agg node,
        // so HAVING / Project / Sort above can reference aggregate expressions.
        std::vector<std::unordered_map<const popo::parser::Expr *, popo::type::Value>> row_aggs;

        size_t width() const;
        std::vector<std::string> flat_names() const;
    };

    struct EvalEnv
    {
        const Batch * layout = nullptr;
        const popo::type::Row * row = nullptr;
        const EvalEnv * outer = nullptr;
        const std::unordered_map<const popo::parser::Expr *, popo::type::Value> * aggs = nullptr;
    };

    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    popo::planner::Planner planner_;
    popo::txn::Transaction * txn_ = nullptr;
    bool own_txn_ = true;
    popo::bytecode::ExecuteSuccess result_;
    std::unordered_map<const popo::parser::Expr *, popo::planner::PlanNodePtr> subquery_plans_;
    std::unordered_map<const popo::planner::PlanNode *, Batch> materialized_;
    std::unordered_map<std::string, Batch> recursive_inputs_;

    Batch exec_batch(const popo::planner::PlanNodePtr & node, const EvalEnv * outer);
    Batch exec_scan(const popo::planner::ScanNode & node);
    Batch exec_subquery_scan(const popo::planner::SubqueryScanNode & node);
    Batch exec_filter(const popo::planner::FilterNode & node, const EvalEnv * outer);
    Batch exec_project(const popo::planner::ProjectNode & node, const EvalEnv * outer);
    Batch exec_join(const popo::planner::JoinNode & node, const EvalEnv * outer);
    Batch exec_agg(const popo::planner::AggNode & node, const EvalEnv * outer);
    Batch exec_sort(const popo::planner::SortNode & node, const EvalEnv * outer);
    Batch exec_limit(const popo::planner::LimitNode & node, const EvalEnv * outer);
    Batch exec_distinct(const popo::planner::DistinctNode & node, const EvalEnv * outer);
    Batch exec_set_op(const popo::planner::SetOpNode & node, const EvalEnv * outer);
    Batch exec_window(const popo::planner::WindowNode & node, const EvalEnv * outer);
    Batch exec_recursive_scan(const popo::planner::RecursiveScanNode & node);
    Batch exec_recursive(const popo::planner::SubqueryScanNode & node);

    void exec_insert(const popo::planner::InsertNode & node);
    void exec_update(const popo::planner::UpdateNode & node);
    void exec_delete(const popo::planner::DeleteNode & node);
    void exec_alter_table(const popo::planner::AlterTableNode & node);
    void exec_create_view(const popo::planner::CreateViewNode & node);
    void exec_drop_view(const popo::planner::DropViewNode & node);
    void rebuild_table(
        const std::string & table_name, const std::vector<popo::catalog::Column> & new_columns,
        const std::vector<popo::type::Row> & rows, const std::string & new_name);

    popo::type::Value eval(const popo::parser::ExprPtr & expr, const EvalEnv & env);
    popo::type::Value eval_quantified(const popo::parser::QuantifiedExpr & expr, const EvalEnv & env);
    popo::type::Value eval_scalar(const popo::parser::ScalarCallExpr & expr, const EvalEnv & env);
    popo::type::Value eval_subquery(const popo::parser::Expr & expr, const EvalEnv & env);
    const popo::planner::PlanNodePtr & subquery_plan(const popo::parser::Expr & expr, const popo::parser::StmtPtr & query);
    popo::type::Value resolve_column(const std::string & table, const std::string & column, const EvalEnv & env) const;
};

} // namespace popo::executor