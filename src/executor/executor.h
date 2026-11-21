#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "base/error.h"
#include "bytecode/vm.h"
#include "catalog/schema_mgr.h"
#include "executor/spill/memory_tracker.h"
#include "executor/spill/temp_file_manager.h"
#include "executor/types/batch.h"
#include "executor/types/cached_subquery.h"
#include "executor/types/column_schema.h"
#include "executor/types/correlated_subquery_cache.h"
#include "executor/types/decorrelated_exists.h"
#include "executor/types/decorrelated_scalar_agg.h"
#include "executor/types/eval_env.h"
#include "executor/types/executor_options.h"
#include "executor/types/executor_spill_stats.h"
#include "executor/types/stream_item.h"
#include "executor/types/stream_materialized.h"
#include "executor/vector/columnar_batch.h"
#include "function/fast_like.h"
#include "parser/ast.h"
#include "planner/planner.h"
#include "storage/storage_engine.h"
#include "type/tuple.h"
#include "type/value.h"

namespace popo::executor
{

// True when the statement uses syntax the bytecode compiler does not support
// (subqueries, CTE, UNION, DISTINCT, OFFSET, CASE, IN/BETWEEN/LIKE/IS NULL,
// aliases, derived tables, FROM-less SELECT, ALTER TABLE, GROUP BY, multi-joins).
bool needs_generic_executor(const popo::parser::StmtPtr & stmt);

// True when planning expanded a view, CTE, or other node the bytecode VM does not run.
bool plan_needs_generic_executor(const popo::planner::PlanNodePtr & plan);

// Vectorized columnar & batch plan executor with SIMD filtering/aggregation,
// DP join ordering, subquery decorrelation, and budget-bounded spilling.
class Executor
{
public:
    using Options = ExecutorOptions;
    using SpillStats = ExecutorSpillStats;

    Executor(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine);
    Executor(
        popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine,
        Options options);
    const SpillStats & spill_stats() const noexcept { return spill_stats_; }

    // outer_txn == nullptr: this call begins and commits one transaction.
    popo::base::Result<popo::bytecode::ExecuteSuccess>
    execute(const popo::planner::PlanNodePtr & plan, popo::txn::Transaction * outer_txn = nullptr);

private:
    using StreamConsumer =
        std::function<popo::base::Result<bool>(StreamItem &&, const Batch &)>;

    popo::catalog::SchemaManager & schema_mgr_;
    popo::storage::StorageEngine & engine_;
    popo::planner::Planner planner_;
    popo::txn::Transaction * txn_ = nullptr;
    bool own_txn_ = true;
    popo::bytecode::ExecuteSuccess result_;
    std::unordered_map<const popo::parser::Expr *, popo::planner::PlanNodePtr> subquery_plans_;
    std::unordered_map<const popo::parser::Expr *, CachedSubquery> uncorrelated_subqueries_;
    std::unordered_map<const popo::parser::Expr *, CorrelatedSubqueryCache> correlated_subqueries_;
    std::unordered_map<const popo::parser::Expr *, DecorrelatedExists> decorrelated_exists_;
    std::unordered_map<const popo::parser::Expr *, DecorrelatedScalarAgg> decorrelated_scalar_aggs_;
    std::unordered_map<const popo::parser::Expr *, popo::function::FastLike> compiled_likes_;
    std::unordered_map<const popo::planner::PlanNode *, Batch> materialized_;
    std::unordered_map<const popo::planner::PlanNode *, StreamMaterialized> stream_materialized_;
    std::unordered_map<std::string, Batch> recursive_inputs_;
    mutable int min_env_depth_seen_ = std::numeric_limits<int>::max();
    mutable std::vector<std::tuple<std::string, std::string, popo::type::Value>> * correlation_capture_ = nullptr;
    mutable int correlation_capture_max_depth_ = -1;
    Options options_;
    SpillStats spill_stats_;
    std::unique_ptr<MemoryTracker> memory_;
    std::unique_ptr<TempFileManager> temp_;
    bool allow_decorrelation_ = false;

    popo::planner::PlanNodePtr optimize_plan(const popo::planner::PlanNodePtr & node, bool preserve_star_order = false) const;
    popo::planner::PlanNodePtr optimize_join_group(const popo::planner::PlanNodePtr & node, bool preserve_star_order) const;
    popo::planner::PlanNodePtr push_projections(const popo::planner::PlanNodePtr & node) const;
    size_t estimate_plan_rows(const popo::planner::PlanNodePtr & node) const;
    static bool collect_schemas(const popo::planner::PlanNodePtr & node, std::vector<ColumnSchema> & out);

    popo::base::Result<Batch> exec_batch(const popo::planner::PlanNodePtr & node, const EvalEnv * outer);
    popo::base::Result<Batch> stream_rows(
        const popo::planner::PlanNodePtr & node, const EvalEnv * outer,
        const StreamConsumer & consumer);
    popo::base::Result<Batch> stream_scan_filter_vectorized(
        const popo::planner::ScanNode & scan,
        const popo::parser::ExprPtr & filter_cond,
        const EvalEnv * outer,
        const std::function<popo::base::Result<bool>(ColumnarBatch &, const Batch &)> & batch_consumer);
    popo::base::Result<std::optional<Batch>> try_exec_vectorized_agg(
        const popo::planner::AggNode & node, const EvalEnv * outer,
        const popo::parser::ExprPtr & having_cond = nullptr);
    popo::base::Result<Batch> stream_join(
        const popo::planner::JoinNode & node, const EvalEnv * outer,
        const StreamConsumer & consumer);
    popo::base::Result<Batch> exec_topn_sort(
        const popo::planner::SortNode & sort, int64_t limit_count, int64_t offset_count, const EvalEnv * outer);
    popo::base::Result<std::optional<popo::type::Value>> try_eval_decorrelated_exists(
        const popo::parser::ExistsExpr & ex, const EvalEnv & env);
    popo::base::Result<std::optional<popo::type::Value>> try_eval_decorrelated_scalar_agg(
        const popo::parser::SubqueryExpr & sub, const EvalEnv & env);
    popo::base::Result<Batch> exec_scan(const popo::planner::ScanNode & node);
    popo::base::Result<Batch> exec_subquery_scan(const popo::planner::SubqueryScanNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_filter(const popo::planner::FilterNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_project(const popo::planner::ProjectNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_join(const popo::planner::JoinNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_agg(const popo::planner::AggNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_sort(const popo::planner::SortNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_limit(const popo::planner::LimitNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_distinct(const popo::planner::DistinctNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_set_op(const popo::planner::SetOpNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_window(const popo::planner::WindowNode & node, const EvalEnv * outer);
    popo::base::Result<Batch> exec_recursive_scan(const popo::planner::RecursiveScanNode & node);
    popo::base::Result<Batch> exec_recursive(const popo::planner::SubqueryScanNode & node);

    popo::base::Result<void> exec_insert(const popo::planner::InsertNode & node);
    popo::base::Result<void> exec_update(const popo::planner::UpdateNode & node);
    popo::base::Result<void> exec_delete(const popo::planner::DeleteNode & node);
    popo::base::Result<void> exec_alter_table(const popo::planner::AlterTableNode & node);
    popo::base::Result<void> exec_create_view(const popo::planner::CreateViewNode & node);
    popo::base::Result<void> exec_drop_view(const popo::planner::DropViewNode & node);
    popo::base::Result<void> rebuild_table(
        const std::string & table_name, const std::vector<popo::catalog::Column> & new_columns,
        const std::vector<popo::type::Row> & rows, const std::string & new_name);

    popo::base::Result<CachedSubquery *> eval_subquery_cached(
        const popo::parser::Expr & expr, const popo::parser::StmtPtr & query, const EvalEnv & env, Batch & scratch);

    popo::base::Result<popo::type::Value> eval(const popo::parser::ExprPtr & expr, const EvalEnv & env);
    popo::base::Result<popo::type::Value> eval_quantified(const popo::parser::QuantifiedExpr & expr, const EvalEnv & env);
    popo::base::Result<popo::type::Value> eval_scalar(const popo::parser::ScalarCallExpr & expr, const EvalEnv & env);
    popo::base::Result<popo::type::Value> eval_subquery(const popo::parser::Expr & expr, const EvalEnv & env);
    popo::base::Result<popo::planner::PlanNodePtr>
    subquery_plan(const popo::parser::Expr & expr, const popo::parser::StmtPtr & query);
    popo::base::Result<popo::type::Value>
    resolve_column(const std::string & table, const std::string & column, const EvalEnv & env) const;
};

} // namespace popo::executor
