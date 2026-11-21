#include "executor/executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/spill_codec.h"
#include "executor/vector/vector_constants.h"
#include "executor/vector/vector_pred.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::is_integer_value;
using popo::function::is_null;
using popo::function::make_bool;
using popo::function::sql_eq;
using popo::function::to_i64;
using popo::function::value_key;
using popo::parser::Expr;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<popo::planner::PlanNodePtr>
Executor::subquery_plan(const popo::parser::Expr & expr, const popo::parser::StmtPtr & query)
{
    auto it = subquery_plans_.find(&expr);
    if (it == subquery_plans_.end())
    {
        auto planned = planner_.plan(query);
        if (!planned)
            return std::unexpected(planned.error());
        it = subquery_plans_.emplace(&expr, push_projections(optimize_plan(*planned, false))).first;
    }
    return it->second;
}

popo::base::Result<std::optional<Value>>
Executor::try_eval_decorrelated_exists(const popo::parser::ExistsExpr & ex, const EvalEnv & env)
{
    if (!allow_decorrelation_ || !ex.query || ex.query->type != popo::parser::StmtType::Select)
        return std::optional<Value>{};

    auto it = decorrelated_exists_.find(&ex);
    if (it == decorrelated_exists_.end())
    {
        DecorrelatedExists cache;
        const auto & sel = static_cast<const popo::parser::SelectStatement &>(*ex.query);
        if (sel.from && sel.where && sel.group_by.empty() && !sel.having && !sel.limit
            && !sel.offset && sel.set_ops.empty() && sel.with_clause.empty())
        {
            std::vector<ColumnSchema> inner_schemas;
            std::vector<ExprPtr> join_conds;
            if (collect_from_table_schemas(sel.from, schema_mgr_, inner_schemas, join_conds))
            {
                bool join_conds_ok = true;
                for (const auto & jc : join_conds)
                {
                    int side = classify_corr_expr(jc, inner_schemas, env);
                    if (side != 0 && side != 1)
                    {
                        join_conds_ok = false;
                        break;
                    }
                }
                if (join_conds_ok)
                {
                    ExprPtr folded_where = fold_constants(sel.where);
                    std::vector<ExprPtr> conjuncts;
                    split_and_conjuncts(folded_where, conjuncts);

                    std::vector<ExprPtr> pure_inner;
                    std::vector<std::pair<ExprPtr, ExprPtr>> eq_corrs;
                    std::vector<std::pair<ExprPtr, ExprPtr>> ne_corrs;
                    bool ok = true;

                    for (const auto & c : conjuncts)
                    {
                        int side = classify_corr_expr(c, inner_schemas, env);
                        if (side == 0 || side == 1)
                        {
                            pure_inner.push_back(c);
                            continue;
                        }
                        if (c->type == ExprType::BinaryOp)
                        {
                            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*c);
                            if (b.op == popo::parser::BinaryOpType::Eq || b.op == popo::parser::BinaryOpType::Ne)
                            {
                                int sl = classify_corr_expr(b.left, inner_schemas, env);
                                int sr = classify_corr_expr(b.right, inner_schemas, env);
                                if ((sl == 1 && sr == 2) || (sl == 2 && sr == 1))
                                {
                                    ExprPtr in_e = (sl == 1) ? b.left : b.right;
                                    ExprPtr out_e = (sl == 1) ? b.right : b.left;
                                    if (b.op == popo::parser::BinaryOpType::Eq)
                                        eq_corrs.emplace_back(std::move(in_e), std::move(out_e));
                                    else
                                        ne_corrs.emplace_back(std::move(in_e), std::move(out_e));
                                    continue;
                                }
                            }
                        }
                        ok = false;
                        break;
                    }

                    if (ok && eq_corrs.size() == 1 && ne_corrs.size() <= 1)
                    {
                        auto rewritten = std::make_shared<popo::parser::SelectStatement>();
                        rewritten->from = sel.from;
                        rewritten->select_list.push_back(eq_corrs[0].first);
                        if (!ne_corrs.empty())
                            rewritten->select_list.push_back(ne_corrs[0].first);
                        for (const auto & pi : pure_inner)
                            rewritten->where = rewritten->where ? combine_and(rewritten->where, pi) : pi;

                        auto planned = planner_.plan(rewritten);
                        if (planned)
                        {
                            auto opt = push_projections(optimize_plan(*planned, false));
                            bool all_i64 = true;
                            bool has_ne = !ne_corrs.empty();
                            std::vector<int64_t> raw_i64_eq;
                            std::vector<std::pair<int64_t, int64_t>> raw_i64_ne;
                            std::vector<std::string> raw_str_eq;
                            bool ne_all_i64 = true;

                            const int saved_min_depth = min_env_depth_seen_;
                            auto * saved_cap = correlation_capture_;
                            const int saved_cap_depth = correlation_capture_max_depth_;
                            min_env_depth_seen_ = std::numeric_limits<int>::max();
                            correlation_capture_ = nullptr;
                            correlation_capture_max_depth_ = -1;

                            bool did_vector_fast = false;
                            popo::base::Result<Batch> streamed;
                            if (opt && opt->type == popo::planner::PlanNodeType::Project)
                            {
                                const auto & proj = static_cast<const popo::planner::ProjectNode &>(*opt);
                                const popo::planner::ScanNode * iscan = nullptr;
                                ExprPtr ifilter;
                                if (extract_scan_and_filters(proj.child, iscan, ifilter)
                                    && proj.expressions.size() == (has_ne ? 2u : 1u)
                                    && proj.expressions[0]->type == ExprType::ColumnRef
                                    && (!has_ne || proj.expressions[1]->type == ExprType::ColumnRef))
                                {
                                    auto iqr = schema_mgr_.query_table(iscan->table_name);
                                    if (iqr && *iqr)
                                    {
                                        const auto & ischema = *iqr;
                                        const std::string itbl = iscan->alias.empty() ? iscan->table_name : iscan->alias;
                                        std::vector<std::string> icols;
                                        std::vector<ValueType> itypes;
                                        auto add_c = [&](size_t idx)
                                        {
                                            if (idx < ischema->columns_.size())
                                            {
                                                icols.push_back(
                                                    (!iscan->column_names.empty() && idx < iscan->column_names.size())
                                                        ? iscan->column_names[idx] : ischema->columns_[idx].column_name_);
                                                itypes.push_back(ischema->columns_[idx].data_type_);
                                            }
                                        };
                                        if (iscan->has_projection)
                                            for (size_t idx : iscan->projected_columns) add_c(idx);
                                        else
                                            for (size_t idx = 0; idx < ischema->columns_.size(); ++idx) add_c(idx);

                                        int c0 = resolve_single_scan_col(
                                            itbl, iscan->alias, icols,
                                            static_cast<const popo::parser::ColumnRefExpr &>(*proj.expressions[0]));
                                        int c1 = has_ne
                                            ? resolve_single_scan_col(
                                                itbl, iscan->alias, icols,
                                                static_cast<const popo::parser::ColumnRefExpr &>(*proj.expressions[1]))
                                            : -1;
                                        if (c0 >= 0 && col_kind_for(itypes[static_cast<size_t>(c0)]) == ColKind::Int64
                                            && (!has_ne || (c1 >= 0 && col_kind_for(itypes[static_cast<size_t>(c1)]) == ColKind::Int64)))
                                        {
                                            did_vector_fast = true;
                                            if (!has_ne)
                                                cache.i64_keys.reserve(16384);
                                            else
                                                cache.i64_ne_map.reserve(16384);
                                            streamed = stream_scan_filter_vectorized(
                                                *iscan, ifilter, nullptr,
                                                [&](ColumnarBatch & cb, const Batch &) -> popo::base::Result<bool>
                                                {
                                                    const auto & col0 = cb.columns[static_cast<size_t>(c0)];
                                                    const int64_t * d0 = col0.i64_data.data();
                                                    const size_t active = cb.has_sel ? cb.sel_count : cb.count;
                                                    if (!has_ne)
                                                    {
                                                        if (!col0.has_nulls)
                                                        {
                                                            if (cb.has_sel)
                                                                for (size_t s = 0; s < active; ++s)
                                                                    cache.i64_keys.insert(d0[cb.sel[s]]);
                                                            else
                                                                for (size_t i = 0; i < active; ++i)
                                                                    cache.i64_keys.insert(d0[i]);
                                                        }
                                                        else
                                                        {
                                                            for (size_t s = 0; s < active; ++s)
                                                            {
                                                                const size_t r = cb.has_sel ? cb.sel[s] : s;
                                                                if (!col0.nulls[r])
                                                                    cache.i64_keys.insert(d0[r]);
                                                            }
                                                        }
                                                    }
                                                    else
                                                    {
                                                        const auto & col1 = cb.columns[static_cast<size_t>(c1)];
                                                        const int64_t * d1 = col1.i64_data.data();
                                                        for (size_t s = 0; s < active; ++s)
                                                        {
                                                            const size_t r = cb.has_sel ? cb.sel[s] : s;
                                                            if ((col0.has_nulls && col0.nulls[r]) || (col1.has_nulls && col1.nulls[r]))
                                                                continue;
                                                            bool ins = false;
                                                            auto & slot = cache.i64_ne_map.get_or_insert(d0[r], ins);
                                                            if (slot.count == 0)
                                                            {
                                                                slot.first = d1[r];
                                                                slot.count = 1;
                                                            }
                                                            else if (slot.count == 1 && slot.first != d1[r])
                                                            {
                                                                slot.count = 2;
                                                            }
                                                        }
                                                    }
                                                    return true;
                                                });
                                        }
                                    }
                                }
                            }

                            if (!did_vector_fast)
                            {
                                streamed = stream_rows(
                                    opt, nullptr,
                                    [&](StreamItem && item, const Batch &) -> popo::base::Result<bool>
                                    {
                                        if (item.row.values_.empty() || is_null(item.row.values_[0]))
                                            return true;
                                        const Value & v0 = item.row.values_[0];
                                        if (!has_ne)
                                        {
                                            if (all_i64 && is_integer_value(v0))
                                            {
                                                raw_i64_eq.push_back(to_i64(v0));
                                            }
                                            else
                                            {
                                                if (all_i64)
                                                {
                                                    all_i64 = false;
                                                    raw_str_eq.reserve(raw_i64_eq.size() + 1);
                                                    for (int64_t k : raw_i64_eq)
                                                        raw_str_eq.push_back(value_key(Value(ValueType::Int64, k)));
                                                    raw_i64_eq.clear();
                                                }
                                                raw_str_eq.push_back(value_key(v0));
                                            }
                                        }
                                        else
                                        {
                                            if (item.row.values_.size() < 2 || is_null(item.row.values_[1]))
                                                return true;
                                            const Value & v1 = item.row.values_[1];
                                            if (ne_all_i64 && is_integer_value(v0) && is_integer_value(v1))
                                            {
                                                raw_i64_ne.emplace_back(to_i64(v0), to_i64(v1));
                                            }
                                            else
                                            {
                                                ne_all_i64 = false;
                                                return false;
                                            }
                                        }
                                        return true;
                                    });
                            }
                            min_env_depth_seen_ = saved_min_depth;
                            correlation_capture_ = saved_cap;
                            correlation_capture_max_depth_ = saved_cap_depth;

                            if (streamed && (!has_ne || ne_all_i64))
                            {
                                cache.eligible = true;
                                cache.has_ne_residual = has_ne;
                                cache.outer_eq_expr = eq_corrs[0].second;
                                if (did_vector_fast)
                                {
                                    cache.key_is_i64 = true;
                                    if (has_ne)
                                        cache.outer_ne_expr = ne_corrs[0].second;
                                }
                                else if (!has_ne)
                                {
                                    cache.key_is_i64 = all_i64;
                                    if (all_i64)
                                    {
                                        cache.i64_keys.reserve(raw_i64_eq.size());
                                        for (int64_t k : raw_i64_eq)
                                            cache.i64_keys.insert(k);
                                    }
                                    else
                                    {
                                        cache.str_keys.reserve(raw_str_eq.size());
                                        for (auto & s : raw_str_eq)
                                            cache.str_keys.insert(std::move(s));
                                    }
                                }
                                else
                                {
                                    cache.key_is_i64 = true;
                                    cache.outer_ne_expr = ne_corrs[0].second;
                                    cache.i64_ne_map.reserve(raw_i64_ne.size() / 2 + 16);
                                    for (const auto & [ek, nk] : raw_i64_ne)
                                    {
                                        bool ins = false;
                                        auto & slot = cache.i64_ne_map.get_or_insert(ek, ins);
                                        if (slot.count == 0)
                                        {
                                            slot.first = nk;
                                            slot.count = 1;
                                        }
                                        else if (slot.count == 1 && slot.first != nk)
                                        {
                                            slot.count = 2;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        it = decorrelated_exists_.emplace(&ex, std::move(cache)).first;
    }

    const auto & cache = it->second;
    if (!cache.eligible)
        return std::optional<Value>{};

    POPO_ASSIGN_OR_RETURN(Value oeq, eval(cache.outer_eq_expr, env));
    if (is_null(oeq))
        return std::optional<Value>(make_bool(false));

    if (!cache.has_ne_residual)
    {
        if (cache.key_is_i64 && is_integer_value(oeq))
            return std::optional<Value>(make_bool(cache.i64_keys.contains(to_i64(oeq))));
        if (!cache.key_is_i64)
            return std::optional<Value>(make_bool(cache.str_keys.contains(value_key(oeq))));
        if (oeq.is_numeric())
        {
            double d = oeq.to_f64();
            if (std::floor(d) == d && d >= -9e18 && d <= 9e18)
                return std::optional<Value>(make_bool(cache.i64_keys.contains(static_cast<int64_t>(d))));
        }
        return std::optional<Value>(make_bool(false));
    }

    POPO_ASSIGN_OR_RETURN(Value one, eval(cache.outer_ne_expr, env));
    if (is_null(one) || !is_integer_value(oeq) || !is_integer_value(one))
        return std::optional<Value>(make_bool(false));
    const auto * entry = cache.i64_ne_map.find(to_i64(oeq));
    if (!entry || entry->count == 0)
        return std::optional<Value>(make_bool(false));
    if (entry->count >= 2)
        return std::optional<Value>(make_bool(true));
    return std::optional<Value>(make_bool(entry->first != to_i64(one)));
}

popo::base::Result<std::optional<Value>>
Executor::try_eval_decorrelated_scalar_agg(const popo::parser::SubqueryExpr & sub, const EvalEnv & env)
{
    if (!allow_decorrelation_ || !sub.query || sub.query->type != popo::parser::StmtType::Select)
        return std::optional<Value>{};

    auto it = decorrelated_scalar_aggs_.find(&sub);
    if (it == decorrelated_scalar_aggs_.end())
    {
        DecorrelatedScalarAgg cache;
        const auto & sel = static_cast<const popo::parser::SelectStatement &>(*sub.query);
        bool saw_agg = false;
        if (sel.from && sel.where && sel.select_list.size() == 1 && sel.group_by.empty()
            && !sel.having && !sel.limit && !sel.offset && sel.set_ops.empty()
            && sel.with_clause.empty() && !sel.distinct
            && expr_contains_agg_only(sel.select_list[0], saw_agg) && saw_agg)
        {
            std::vector<ColumnSchema> inner_schemas;
            std::vector<ExprPtr> join_conds;
            if (collect_from_table_schemas(sel.from, schema_mgr_, inner_schemas, join_conds))
            {
                bool join_conds_ok = true;
                for (const auto & jc : join_conds)
                {
                    int side = classify_corr_expr(jc, inner_schemas, env);
                    if (side != 0 && side != 1)
                    {
                        join_conds_ok = false;
                        break;
                    }
                }
                if (join_conds_ok)
                {
                    ExprPtr folded_where = fold_constants(sel.where);
                    std::vector<ExprPtr> conjuncts;
                    split_and_conjuncts(folded_where, conjuncts);

                    std::vector<ExprPtr> pure_inner;
                    std::vector<std::pair<ExprPtr, ExprPtr>> eq_corrs;
                    bool ok = true;

                    for (const auto & c : conjuncts)
                    {
                        int side = classify_corr_expr(c, inner_schemas, env);
                        if (side == 0 || side == 1)
                        {
                            pure_inner.push_back(c);
                            continue;
                        }
                        if (c->type == ExprType::BinaryOp)
                        {
                            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*c);
                            if (b.op == popo::parser::BinaryOpType::Eq)
                            {
                                int sl = classify_corr_expr(b.left, inner_schemas, env);
                                int sr = classify_corr_expr(b.right, inner_schemas, env);
                                if ((sl == 1 && sr == 2) || (sl == 2 && sr == 1))
                                {
                                    ExprPtr in_e = (sl == 1) ? b.left : b.right;
                                    ExprPtr out_e = (sl == 1) ? b.right : b.left;
                                    eq_corrs.emplace_back(std::move(in_e), std::move(out_e));
                                    continue;
                                }
                            }
                        }
                        ok = false;
                        break;
                    }

                    if (ok && (eq_corrs.size() == 1 || eq_corrs.size() == 2))
                    {
                        std::vector<const popo::parser::FunctionCallExpr *> agg_ptrs;
                        collect_agg_expr_ptrs(sel.select_list[0], agg_ptrs);
                        std::unordered_map<const Expr *, Value> empty_aggs;
                        for (const auto * fn : agg_ptrs)
                        {
                            if (fn->func == popo::parser::AggFuncType::Count)
                                empty_aggs.emplace(fn, Value(ValueType::Int64, int64_t{0}));
                            else
                                empty_aggs.emplace(fn, Value());
                        }
                        Batch empty_layout;
                        Row empty_row;
                        EvalEnv empty_env{&empty_layout, &empty_row, nullptr, &empty_aggs, 0};
                        auto def_res = eval(sel.select_list[0], empty_env);
                        if (def_res)
                        {
                            auto rewritten = std::make_shared<popo::parser::SelectStatement>();
                            rewritten->from = sel.from;
                            for (const auto & [in_e, _] : eq_corrs)
                            {
                                rewritten->select_list.push_back(in_e);
                                rewritten->group_by.push_back(in_e);
                            }
                            rewritten->select_list.push_back(sel.select_list[0]);
                            for (const auto & pi : pure_inner)
                                rewritten->where = rewritten->where ? combine_and(rewritten->where, pi) : pi;

                            auto planned = planner_.plan(rewritten);
                            if (planned)
                            {
                                auto opt = push_projections(optimize_plan(*planned, false));
                                const size_t kcnt = eq_corrs.size();
                                bool all_i64 = true;
                                std::vector<std::pair<int64_t, Value>> rows_1i64;
                                std::vector<std::tuple<int64_t, int64_t, Value>> rows_2i64;
                                std::vector<std::pair<std::string, Value>> rows_str;

                                const int saved_min_depth = min_env_depth_seen_;
                                auto * saved_cap = correlation_capture_;
                                const int saved_cap_depth = correlation_capture_max_depth_;
                                min_env_depth_seen_ = std::numeric_limits<int>::max();
                                correlation_capture_ = nullptr;
                                correlation_capture_max_depth_ = -1;
                                auto streamed = stream_rows(
                                    opt, nullptr,
                                    [&](StreamItem && item, const Batch &) -> popo::base::Result<bool>
                                    {
                                        if (item.row.values_.size() < kcnt + 1)
                                            return true;
                                        for (size_t i = 0; i < kcnt; ++i)
                                            if (is_null(item.row.values_[i]))
                                                return true;
                                        const Value & res_val = item.row.values_[kcnt];
                                        if (kcnt == 1 && all_i64 && is_integer_value(item.row.values_[0]))
                                        {
                                            rows_1i64.emplace_back(to_i64(item.row.values_[0]), res_val);
                                        }
                                        else if (kcnt == 2 && all_i64
                                                 && is_integer_value(item.row.values_[0])
                                                 && is_integer_value(item.row.values_[1]))
                                        {
                                            rows_2i64.emplace_back(
                                                to_i64(item.row.values_[0]), to_i64(item.row.values_[1]), res_val);
                                        }
                                        else
                                        {
                                            if (all_i64)
                                            {
                                                all_i64 = false;
                                                for (auto & [k, v] : rows_1i64)
                                                {
                                                    std::string sk = value_key(Value(ValueType::Int64, k));
                                                    sk.push_back('\x1f');
                                                    rows_str.emplace_back(std::move(sk), std::move(v));
                                                }
                                                rows_1i64.clear();
                                                for (auto & [k1, k2, v] : rows_2i64)
                                                {
                                                    std::string sk = value_key(Value(ValueType::Int64, k1));
                                                    sk.push_back('\x1f');
                                                    sk += value_key(Value(ValueType::Int64, k2));
                                                    sk.push_back('\x1f');
                                                    rows_str.emplace_back(std::move(sk), std::move(v));
                                                }
                                                rows_2i64.clear();
                                            }
                                            std::string sk;
                                            for (size_t i = 0; i < kcnt; ++i)
                                            {
                                                sk += value_key(item.row.values_[i]);
                                                sk.push_back('\x1f');
                                            }
                                            rows_str.emplace_back(std::move(sk), res_val);
                                        }
                                        return true;
                                    });
                                min_env_depth_seen_ = saved_min_depth;
                                correlation_capture_ = saved_cap;
                                correlation_capture_max_depth_ = saved_cap_depth;

                                if (streamed)
                                {
                                    cache.eligible = true;
                                    cache.empty_default = std::move(*def_res);
                                    for (const auto & [_, out_e] : eq_corrs)
                                        cache.outer_key_exprs.push_back(out_e);
                                    cache.keys_are_i64 = all_i64;
                                    if (all_i64 && kcnt == 1)
                                    {
                                        cache.map_1i64.reserve(rows_1i64.size());
                                        for (auto & [k, v] : rows_1i64)
                                        {
                                            bool ins = false;
                                            cache.map_1i64.get_or_insert(k, ins) = std::move(v);
                                        }
                                    }
                                    else if (all_i64 && kcnt == 2)
                                    {
                                        cache.map_2i64.reserve(rows_2i64.size());
                                        for (auto & [k1, k2, v] : rows_2i64)
                                        {
                                            bool ins = false;
                                            cache.map_2i64.get_or_insert(k1, k2, ins) = std::move(v);
                                        }
                                    }
                                    else
                                    {
                                        cache.map_str.reserve(rows_str.size());
                                        for (auto & [sk, v] : rows_str)
                                            cache.map_str.emplace(std::move(sk), std::move(v));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        it = decorrelated_scalar_aggs_.emplace(&sub, std::move(cache)).first;
    }

    const auto & cache = it->second;
    if (!cache.eligible)
        return std::optional<Value>{};

    const size_t kcnt = cache.outer_key_exprs.size();
    if (cache.keys_are_i64 && kcnt == 1)
    {
        POPO_ASSIGN_OR_RETURN(Value k0, eval(cache.outer_key_exprs[0], env));
        if (is_null(k0) || !is_integer_value(k0))
            return std::optional<Value>(cache.empty_default);
        const Value * hit = cache.map_1i64.find(to_i64(k0));
        return std::optional<Value>(hit ? *hit : cache.empty_default);
    }
    if (cache.keys_are_i64 && kcnt == 2)
    {
        POPO_ASSIGN_OR_RETURN(Value k0, eval(cache.outer_key_exprs[0], env));
        if (is_null(k0) || !is_integer_value(k0))
            return std::optional<Value>(cache.empty_default);
        POPO_ASSIGN_OR_RETURN(Value k1, eval(cache.outer_key_exprs[1], env));
        if (is_null(k1) || !is_integer_value(k1))
            return std::optional<Value>(cache.empty_default);
        const Value * hit = cache.map_2i64.find(to_i64(k0), to_i64(k1));
        return std::optional<Value>(hit ? *hit : cache.empty_default);
    }
    std::string sk;
    for (size_t i = 0; i < kcnt; ++i)
    {
        POPO_ASSIGN_OR_RETURN(Value kv, eval(cache.outer_key_exprs[i], env));
        if (is_null(kv))
            return std::optional<Value>(cache.empty_default);
        sk += value_key(kv);
        sk.push_back('\x1f');
    }
    auto hit = cache.map_str.find(sk);
    return std::optional<Value>(hit != cache.map_str.end() ? hit->second : cache.empty_default);
}

popo::base::Result<CachedSubquery *> Executor::eval_subquery_cached(
    const popo::parser::Expr & expr, const popo::parser::StmtPtr & query, const EvalEnv & env, Batch & scratch)
{
    auto hit = uncorrelated_subqueries_.find(&expr);
    if (hit != uncorrelated_subqueries_.end())
        return &hit->second;

    auto correlated_hit = correlated_subqueries_.find(&expr);
    if (correlated_hit != correlated_subqueries_.end()
        && !correlated_hit->second.outer_columns.empty())
    {
        std::string key;
        for (const auto & [table, column] : correlated_hit->second.outer_columns)
        {
            POPO_ASSIGN_OR_RETURN(Value value, resolve_column(table, column, env));
            key += value_key(value);
            key.push_back('\x1f');
        }
        auto entry = correlated_hit->second.entries.find(key);
        if (entry != correlated_hit->second.entries.end())
            return &entry->second;
    }

    int saved = min_env_depth_seen_;
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    std::vector<std::tuple<std::string, std::string, Value>> captured;
    auto * saved_capture = correlation_capture_;
    int saved_capture_depth = correlation_capture_max_depth_;
    correlation_capture_ = &captured;
    correlation_capture_max_depth_ = env.depth;
    auto planned = subquery_plan(expr, query);
    if (!planned)
    {
        correlation_capture_ = saved_capture;
        correlation_capture_max_depth_ = saved_capture_depth;
        return std::unexpected(planned.error());
    }
    auto plan = std::move(*planned);
    auto executed = exec_batch(plan, &env);
    correlation_capture_ = saved_capture;
    correlation_capture_max_depth_ = saved_capture_depth;
    if (!executed)
        return std::unexpected(executed.error());
    scratch = std::move(*executed);
    bool correlated = min_env_depth_seen_ <= env.depth;
    min_env_depth_seen_ = std::min(saved, min_env_depth_seen_);
    if (!correlated)
    {
        CachedSubquery entry;
        entry.batch = std::move(scratch);
        auto [it, _] = uncorrelated_subqueries_.emplace(&expr, std::move(entry));
        return &it->second;
    }
    if (correlated && !captured.empty())
    {
        auto & cache = correlated_subqueries_[&expr];
        if (cache.outer_columns.empty())
        {
            std::set<std::pair<std::string, std::string>> seen;
            for (const auto & [table, column, _] : captured)
                if (seen.emplace(table, column).second)
                    cache.outer_columns.emplace_back(table, column);
        }
        std::string key;
        for (const auto & [table, column] : cache.outer_columns)
        {
            bool found = false;
            for (const auto & [captured_table, captured_column, value] : captured)
            {
                if (captured_table == table && captured_column == column)
                {
                    key += value_key(value);
                    key.push_back('\x1f');
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                POPO_ASSIGN_OR_RETURN(Value value, resolve_column(table, column, env));
                key += value_key(value);
                key.push_back('\x1f');
            }
        }
        size_t bytes = sizeof(CachedSubquery);
        for (const auto & row : scratch.rows)
            bytes += estimate_row_bytes(row);
        if ((!memory_ || memory_->try_reserve(bytes)) && cache.entries.size() < 100000)
        {
            CachedSubquery entry;
            entry.batch = std::move(scratch);
            auto [it, _] = cache.entries.emplace(std::move(key), std::move(entry));
            return &it->second;
        }
    }
    return nullptr;
}

popo::base::Result<Value> Executor::eval_subquery(const popo::parser::Expr & expr, const EvalEnv & env)
{
    switch (expr.type)
    {
        case ExprType::Subquery: {
            const auto & sub = static_cast<const popo::parser::SubqueryExpr &>(expr);
            {
                POPO_ASSIGN_OR_RETURN(auto decorr, try_eval_decorrelated_scalar_agg(sub, env));
                if (decorr)
                    return std::move(*decorr);
            }
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, sub.query, env, scratch));
            const Batch & batch = cached ? cached->batch : scratch;
            if (batch.rows.empty())
                return Value();
            if (batch.rows.size() > 1)
                return executor_error("scalar subquery returned more than one row");
            if (batch.rows[0].values_.empty())
                return executor_error("scalar subquery must return one column");
            return batch.rows[0].values_[0];
        }
        case ExprType::InSubquery: {
            const auto & in = static_cast<const popo::parser::InSubqueryExpr &>(expr);
            POPO_ASSIGN_OR_RETURN(Value operand, eval(in.operand, env));
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, in.query, env, scratch));
            if (cached)
            {
                if (!cached->indexed)
                {
                    bool all_i64 = true;
                    for (const auto & row : cached->batch.rows)
                    {
                        if (row.values_.empty() || is_null(row.values_[0]))
                            cached->has_null = true;
                        else if (!is_integer_value(row.values_[0]))
                            all_i64 = false;
                    }
                    if (all_i64)
                    {
                        cached->first_col_is_i64 = true;
                        cached->first_col_i64.reserve(cached->batch.rows.size());
                        for (const auto & row : cached->batch.rows)
                        {
                            if (!row.values_.empty() && !is_null(row.values_[0]))
                                cached->first_col_i64.insert(to_i64(row.values_[0]));
                        }
                    }
                    else
                    {
                        cached->first_col_keys.reserve(cached->batch.rows.size());
                        for (const auto & row : cached->batch.rows)
                        {
                            if (!row.values_.empty() && !is_null(row.values_[0]))
                                cached->first_col_keys.insert(value_key(row.values_[0]));
                        }
                    }
                    cached->indexed = true;
                }
                if (cached->batch.rows.empty())
                    return make_bool(in.negated);
                if (is_null(operand))
                    return make_bool(false);
                bool found = false;
                if (cached->first_col_is_i64)
                {
                    if (is_integer_value(operand))
                        found = cached->first_col_i64.contains(to_i64(operand));
                    else if (operand.is_numeric())
                    {
                        double d = operand.to_f64();
                        if (std::floor(d) == d && d >= -9e18 && d <= 9e18)
                            found = cached->first_col_i64.contains(static_cast<int64_t>(d));
                    }
                }
                else
                {
                    found = cached->first_col_keys.contains(value_key(operand));
                }
                if (found)
                    return make_bool(!in.negated);
                if (in.negated && cached->has_null)
                    return make_bool(false);
                return make_bool(in.negated);
            }

            const Batch & batch = scratch;
            if (batch.rows.empty())
                return make_bool(in.negated);
            if (is_null(operand))
                return make_bool(false);
            bool found = false;
            bool saw_null = false;
            for (const auto & row : batch.rows)
            {
                if (row.values_.empty() || is_null(row.values_[0]))
                {
                    saw_null = true;
                    continue;
                }
                if (sql_eq(operand, row.values_[0]))
                {
                    found = true;
                    break;
                }
            }
            if (found)
                return make_bool(!in.negated);
            if (in.negated && saw_null)
                return make_bool(false);
            return make_bool(in.negated);
        }
        case ExprType::Exists: {
            const auto & ex = static_cast<const popo::parser::ExistsExpr &>(expr);
            {
                POPO_ASSIGN_OR_RETURN(auto decorr, try_eval_decorrelated_exists(ex, env));
                if (decorr)
                    return std::move(*decorr);
            }
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, ex.query, env, scratch));
            const Batch & batch = cached ? cached->batch : scratch;
            return make_bool(!batch.rows.empty());
        }
        default:
            return executor_error("not a subquery expression", popo::base::Errno::InternalErr);
    }
}

} // namespace popo::executor
