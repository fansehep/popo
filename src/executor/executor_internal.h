#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/error.h"
#include "catalog/schema_mgr.h"
#include "executor/types/column_schema.h"
#include "executor/types/eval_env.h"
#include "executor/types/executor_options.h"
#include "parser/ast.h"
#include "planner/plan_node.h"
#include "type/value.h"

#define POPO_EXEC_CONCAT_INNER(a, b) a##b
#define POPO_EXEC_CONCAT(a, b) POPO_EXEC_CONCAT_INNER(a, b)
#define POPO_ASSIGN_OR_RETURN_IMPL(lhs, expression, n)                         \
    auto POPO_EXEC_CONCAT(_popo_result_, n) = (expression);                    \
    if (!POPO_EXEC_CONCAT(_popo_result_, n))                                   \
        return std::unexpected(POPO_EXEC_CONCAT(_popo_result_, n).error());    \
    lhs = std::move(*POPO_EXEC_CONCAT(_popo_result_, n))
#define POPO_ASSIGN_OR_RETURN(lhs, expression) POPO_ASSIGN_OR_RETURN_IMPL(lhs, expression, __COUNTER__)

namespace popo::executor::detail
{

std::unexpected<popo::base::Error> executor_error(
    std::string message, popo::base::Errno code = popo::base::Errno::InvalidArg);

ExecutorOptions default_executor_options();

void split_and_conjuncts(const popo::parser::ExprPtr & expr, std::vector<popo::parser::ExprPtr> & out);
void split_or_branches(const popo::parser::ExprPtr & expr, std::vector<popo::parser::ExprPtr> & out);
bool same_col_ref(const popo::parser::ExprPtr & a, const popo::parser::ExprPtr & b);
bool same_equijoin_expr(const popo::parser::ExprPtr & a, const popo::parser::ExprPtr & b);
void extract_common_or_equijoins(const popo::parser::ExprPtr & expr, std::vector<popo::parser::ExprPtr> & out);

popo::parser::ExprPtr combine_and(const popo::parser::ExprPtr & a, const popo::parser::ExprPtr & b);
popo::parser::ExprPtr combine_and(const std::vector<popo::parser::ExprPtr> & conjuncts);
popo::parser::ExprPtr combine_or(const std::vector<popo::parser::ExprPtr> & branches);
popo::parser::ExprPtr make_literal_expr(popo::type::Value v);
popo::parser::ExprPtr fold_constants(const popo::parser::ExprPtr & expr);

bool expr_has_subquery_or_agg(const popo::parser::ExprPtr & expr);
void collect_col_refs(
    const popo::parser::ExprPtr & expr,
    std::vector<const popo::parser::ColumnRefExpr *> & out);
void collect_col_refs_deep(
    const popo::parser::ExprPtr & expr,
    std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star);
void collect_table_ref_col_refs(
    const popo::parser::TableRefPtr & ref,
    std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star);
void collect_select_col_refs(
    const popo::parser::SelectStatement & sel,
    std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star);
void collect_table_names_from_ref(
    const popo::parser::TableRefPtr & ref, std::vector<std::string> & names);

bool extract_scan_and_filters(
    const popo::planner::PlanNodePtr & node,
    const popo::planner::ScanNode *& out_scan,
    popo::parser::ExprPtr & out_filter);
bool mask_has_any_nonzero(const uint8_t * mask, size_t n) noexcept;
bool expr_is_pure_integer(
    const popo::parser::ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<popo::type::ValueType> & col_types);
bool expr_has_int_div(
    const popo::parser::ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<popo::type::ValueType> & col_types);

void split_join_equi_and_residual(
    const popo::parser::ExprPtr & condition,
    const std::vector<ColumnSchema> & left_schemas,
    const std::vector<ColumnSchema> & right_schemas,
    std::vector<std::pair<popo::parser::ExprPtr, popo::parser::ExprPtr>> & equi_keys,
    popo::parser::ExprPtr & residual_cond);

bool collect_from_table_schemas(
    const popo::parser::TableRefPtr & ref,
    popo::catalog::SchemaManager & schema_mgr,
    std::vector<ColumnSchema> & out,
    std::vector<popo::parser::ExprPtr> & join_conds);

bool col_resolves_in_schemas(
    const std::vector<ColumnSchema> & schemas,
    const std::string & tbl, const std::string & col);

bool col_resolves_in_env_chain(
    const EvalEnv * env,
    const std::string & tbl, const std::string & col);

int classify_corr_expr(
    const popo::parser::ExprPtr & expr,
    const std::vector<ColumnSchema> & inner_schemas,
    const EvalEnv & outer_env);

bool expr_contains_agg_only(const popo::parser::ExprPtr & expr, bool & saw_agg);
void collect_agg_expr_ptrs(
    const popo::parser::ExprPtr & expr,
    std::vector<const popo::parser::FunctionCallExpr *> & out);

} // namespace popo::executor::detail
