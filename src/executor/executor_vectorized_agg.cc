#include "executor/executor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/memory_charge_guard.h"
#include "executor/spill/spill_codec.h"
#include "executor/types/compiled_agg.h"
#include "executor/types/group_accum.h"
#include "executor/vector/aligned_buffer.h"
#include "executor/vector/flat_int64_map.h"
#include "executor/vector/flat_pair64_map.h"
#include "executor/vector/simd_kernels.h"
#include "executor/vector/vector_arith.h"
#include "executor/vector/vector_constants.h"
#include "executor/vector/vector_pred.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::as_bool;
using popo::function::is_null;
using popo::parser::BinaryOpType;
using popo::parser::Expr;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<std::optional<Batch>>
Executor::try_exec_vectorized_agg(
    const popo::planner::AggNode & node, const EvalEnv * outer,
    const ExprPtr & having_cond)
{
    const popo::planner::ScanNode * scan = nullptr;
    ExprPtr filter_cond;
    if (!extract_scan_and_filters(node.child, scan, filter_cond))
        return std::optional<Batch>{};

    auto qr = schema_mgr_.query_table(scan->table_name);
    if (!qr || !*qr)
        return std::optional<Batch>{};
    const auto & schema = *qr;
    const size_t total_phys_cols = schema->columns_.size();
    std::vector<uint16_t> proj_cols;
    if (scan->has_projection)
        proj_cols = scan->projected_columns;
    else
    {
        proj_cols.resize(total_phys_cols);
        for (size_t i = 0; i < total_phys_cols; ++i)
            proj_cols[i] = static_cast<uint16_t>(i);
    }

    const std::string scan_table = scan->alias.empty() ? scan->table_name : scan->alias;
    std::vector<std::string> col_names;
    std::vector<ValueType> col_types;
    col_names.reserve(proj_cols.size());
    col_types.reserve(proj_cols.size());
    for (size_t idx : proj_cols)
    {
        if (idx < total_phys_cols)
        {
            col_names.push_back(
                (!scan->column_names.empty() && idx < scan->column_names.size())
                    ? scan->column_names[idx]
                    : schema->columns_[idx].column_name_);
            col_types.push_back(schema->columns_[idx].data_type_);
        }
    }

    // Check group_by shape:
    // Mode 0: global (0 keys, e.g. Q6)
    // Mode 1: 1 Int64 key (e.g. Q15, Q17-decorr, Q18-sub)
    // Mode 2: 2 Int64 keys (e.g. Q20-decorr)
    // Mode 3: 2 single-char String keys (e.g. Q1: l_returnflag, l_linestatus)
    enum class GroupMode : uint8_t { Global, OneI64, TwoI64, TwoChar1 };
    GroupMode mode = GroupMode::Global;
    size_t gk0 = 0;
    size_t gk1 = 0;
    if (node.group_by.size() == 1)
    {
        if (node.group_by[0]->type != ExprType::ColumnRef)
            return std::optional<Batch>{};
        int c0 = resolve_single_scan_col(
            scan_table, scan->alias, col_names,
            static_cast<const popo::parser::ColumnRefExpr &>(*node.group_by[0]));
        if (c0 < 0 || col_kind_for(col_types[static_cast<size_t>(c0)]) != ColKind::Int64)
            return std::optional<Batch>{};
        gk0 = static_cast<size_t>(c0);
        mode = GroupMode::OneI64;
    }
    else if (node.group_by.size() == 2)
    {
        if (node.group_by[0]->type != ExprType::ColumnRef || node.group_by[1]->type != ExprType::ColumnRef)
            return std::optional<Batch>{};
        int c0 = resolve_single_scan_col(
            scan_table, scan->alias, col_names,
            static_cast<const popo::parser::ColumnRefExpr &>(*node.group_by[0]));
        int c1 = resolve_single_scan_col(
            scan_table, scan->alias, col_names,
            static_cast<const popo::parser::ColumnRefExpr &>(*node.group_by[1]));
        if (c0 < 0 || c1 < 0)
            return std::optional<Batch>{};
        const auto k0 = col_kind_for(col_types[static_cast<size_t>(c0)]);
        const auto k1 = col_kind_for(col_types[static_cast<size_t>(c1)]);
        gk0 = static_cast<size_t>(c0);
        gk1 = static_cast<size_t>(c1);
        if (k0 == ColKind::Int64 && k1 == ColKind::Int64)
            mode = GroupMode::TwoI64;
        else if (k0 == ColKind::String && k1 == ColKind::String)
            mode = GroupMode::TwoChar1;
        else
            return std::optional<Batch>{};
    }
    else if (node.group_by.size() > 2)
    {
        return std::optional<Batch>{};
    }

    const size_t n_aggs = node.agg_exprs.size();
    std::vector<CompiledAgg> aggs(n_aggs);
    for (size_t i = 0; i < n_aggs; ++i)
    {
        const auto & fn = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
        if (fn.is_distinct || fn.func == popo::parser::AggFuncType::XmlAgg)
            return std::optional<Batch>{};
        aggs[i].func = fn.func;
        const bool star = fn.args.empty() || fn.args[0]->type == ExprType::Star;
        if (star)
        {
            if (fn.func != popo::parser::AggFuncType::Count)
                return std::optional<Batch>{};
            aggs[i].is_star = true;
            continue;
        }
        if (expr_has_int_div(fn.args[0], scan_table, scan->alias, col_names, col_types))
            return std::optional<Batch>{};
        auto ar = compile_vector_arith(fn.args[0], scan_table, scan->alias, col_names, col_types);
        if (!ar)
            return std::optional<Batch>{};
        if (ar->kind == VectorArith::Kind::Col)
            aggs[i].direct_col = static_cast<int>(ar->col_idx);
        aggs[i].is_int_result = expr_is_pure_integer(fn.args[0], scan_table, scan->alias, col_names, col_types)
            && (fn.func == popo::parser::AggFuncType::Sum
                || fn.func == popo::parser::AggFuncType::Min
                || fn.func == popo::parser::AggFuncType::Max);
        aggs[i].arith = std::move(ar);
    }

    std::vector<AlignedBuffer<double>> val_bufs(n_aggs);
    std::vector<AlignedBuffer<uint8_t>> null_bufs(n_aggs);
    std::vector<bool> arg_has_nulls(n_aggs, false);
    for (size_t a = 0; a < n_aggs; ++a)
    {
        if (!aggs[a].is_star)
        {
            val_bufs[a] = AlignedBuffer<double>(kVectorBatchSize);
            null_bufs[a] = AlignedBuffer<uint8_t>(kVectorBatchSize);
        }
    }

    std::vector<GroupAccum> accums;
    std::vector<Row> group_reps;
    bool aborted_fallback = false;
    MemoryChargeGuard guard(memory_.get());

    auto alloc_group = [&](const ColumnarBatch & b, size_t row_idx, uint32_t & out_gid) -> bool
    {
        Row rep = b.extract_row(row_idx);
        const size_t need = estimate_row_bytes(rep) + n_aggs * sizeof(GroupAccum) + 48;
        if (memory_ && !guard.charge(need))
        {
            aborted_fallback = true;
            return false;
        }
        out_gid = static_cast<uint32_t>(group_reps.size());
        group_reps.push_back(std::move(rep));
        accums.resize((out_gid + 1) * n_aggs);
        return true;
    };

    FlatInt64Map<uint32_t> map_i64;
    FlatPair64Map<uint32_t> map_pair64;
    std::vector<int32_t> char2_table;
    if (mode == GroupMode::TwoChar1)
        char2_table.assign(65536, -1);
    bool have_global_group = false;
    if (mode == GroupMode::Global)
        accums.resize(n_aggs);

    auto update_group_scalar = [&](uint32_t gid, size_t row_idx, const ColumnarBatch & b)
    {
        GroupAccum * g = accums.data() + static_cast<size_t>(gid) * n_aggs;
        for (size_t a = 0; a < n_aggs; ++a)
        {
            const auto & ca = aggs[a];
            if (ca.is_star)
            {
                ++g[a].count;
                continue;
            }
            if (arg_has_nulls[a] && null_bufs[a][row_idx])
                continue;
            const double v = val_bufs[a][row_idx];
            switch (ca.func)
            {
                case popo::parser::AggFuncType::Count:
                    ++g[a].count;
                    break;
                case popo::parser::AggFuncType::Sum:
                case popo::parser::AggFuncType::Avg:
                    if (ca.is_int_result && ca.direct_col >= 0
                        && b.columns[static_cast<size_t>(ca.direct_col)].kind == ColKind::Int64)
                    {
                        g[a].i64_val += b.columns[static_cast<size_t>(ca.direct_col)].i64_data[row_idx];
                    }
                    else
                    {
                        g[a].f64_val += v;
                    }
                    ++g[a].count;
                    g[a].init = true;
                    break;
                case popo::parser::AggFuncType::Min:
                    if (ca.is_int_result && ca.direct_col >= 0
                        && b.columns[static_cast<size_t>(ca.direct_col)].kind == ColKind::Int64)
                    {
                        const int64_t iv = b.columns[static_cast<size_t>(ca.direct_col)].i64_data[row_idx];
                        if (!g[a].init || iv < g[a].i64_val)
                        {
                            g[a].i64_val = iv;
                            g[a].init = true;
                        }
                    }
                    else if (!g[a].init || v < g[a].f64_val)
                    {
                        g[a].f64_val = v;
                        g[a].init = true;
                    }
                    break;
                case popo::parser::AggFuncType::Max:
                    if (ca.is_int_result && ca.direct_col >= 0
                        && b.columns[static_cast<size_t>(ca.direct_col)].kind == ColKind::Int64)
                    {
                        const int64_t iv = b.columns[static_cast<size_t>(ca.direct_col)].i64_data[row_idx];
                        if (!g[a].init || iv > g[a].i64_val)
                        {
                            g[a].i64_val = iv;
                            g[a].init = true;
                        }
                    }
                    else if (!g[a].init || v > g[a].f64_val)
                    {
                        g[a].f64_val = v;
                        g[a].init = true;
                    }
                    break;
                default:
                    break;
            }
        }
    };

    std::vector<uint32_t> batch_gids(kVectorBatchSize);

    auto streamed = stream_scan_filter_vectorized(
        *scan, filter_cond, outer,
        [&](ColumnarBatch & batch, const Batch &) -> popo::base::Result<bool>
        {
            const size_t active_count = batch.has_sel ? batch.sel_count : batch.count;
            if (active_count == 0)
                return true;

            if (mode == GroupMode::TwoChar1)
            {
                const auto & c0 = batch.columns[gk0];
                const auto & c1 = batch.columns[gk1];
                if (!c0.all_len1 || !c1.all_len1 || c0.has_nulls || c1.has_nulls)
                {
                    aborted_fallback = true;
                    return false;
                }
            }

            for (size_t a = 0; a < n_aggs; ++a)
            {
                if (aggs[a].is_star)
                    continue;
                bool any_null = false;
                eval_vector_arith(*aggs[a].arith, batch, val_bufs[a].data(), null_bufs[a].data(), any_null);
                arg_has_nulls[a] = any_null;
            }

            if (mode == GroupMode::Global)
            {
                have_global_group = true;
                GroupAccum * g = accums.data();
                for (size_t a = 0; a < n_aggs; ++a)
                {
                    const auto & ca = aggs[a];
                    if (ca.is_star)
                    {
                        g[a].count += static_cast<int64_t>(active_count);
                        continue;
                    }
                    const bool no_nulls = !arg_has_nulls[a];
                    if (no_nulls && (ca.func == popo::parser::AggFuncType::Sum || ca.func == popo::parser::AggFuncType::Avg))
                    {
                        if (ca.is_int_result && ca.direct_col >= 0
                            && batch.columns[static_cast<size_t>(ca.direct_col)].kind == ColKind::Int64)
                        {
                            const int64_t * col_i64 = batch.columns[static_cast<size_t>(ca.direct_col)].i64_data.data();
                            if (batch.has_sel)
                                g[a].i64_val += simd_sum_i64_sel(col_i64, batch.sel.data(), batch.sel_count);
                            else
                            {
                                int64_t s = 0;
                                for (size_t i = 0; i < batch.count; ++i)
                                    s += col_i64[i];
                                g[a].i64_val += s;
                            }
                        }
                        else
                        {
                            if (batch.has_sel)
                                g[a].f64_val += simd_sum_f64_sel(val_bufs[a].data(), batch.sel.data(), batch.sel_count);
                            else
                                g[a].f64_val += simd_sum_f64_all(val_bufs[a].data(), batch.count);
                        }
                        g[a].count += static_cast<int64_t>(active_count);
                        g[a].init = true;
                        continue;
                    }
                    if (no_nulls && ca.func == popo::parser::AggFuncType::Count)
                    {
                        g[a].count += static_cast<int64_t>(active_count);
                        continue;
                    }
                    const bool is_int_col = ca.is_int_result && ca.direct_col >= 0
                        && batch.columns[static_cast<size_t>(ca.direct_col)].kind == ColKind::Int64;
                    const int64_t * col_i64 = is_int_col
                        ? batch.columns[static_cast<size_t>(ca.direct_col)].i64_data.data()
                        : nullptr;
                    auto accum_one = [&](size_t i)
                    {
                        if (arg_has_nulls[a] && null_bufs[a][i])
                            return;
                        const double v = val_bufs[a][i];
                        if (ca.func == popo::parser::AggFuncType::Count)
                        {
                            ++g[a].count;
                        }
                        else if (ca.func == popo::parser::AggFuncType::Sum || ca.func == popo::parser::AggFuncType::Avg)
                        {
                            if (is_int_col)
                                g[a].i64_val += col_i64[i];
                            else
                                g[a].f64_val += v;
                            ++g[a].count;
                            g[a].init = true;
                        }
                        else if (ca.func == popo::parser::AggFuncType::Min)
                        {
                            if (is_int_col)
                            {
                                if (!g[a].init || col_i64[i] < g[a].i64_val) { g[a].i64_val = col_i64[i]; g[a].init = true; }
                            }
                            else if (!g[a].init || v < g[a].f64_val) { g[a].f64_val = v; g[a].init = true; }
                        }
                        else if (ca.func == popo::parser::AggFuncType::Max)
                        {
                            if (is_int_col)
                            {
                                if (!g[a].init || col_i64[i] > g[a].i64_val) { g[a].i64_val = col_i64[i]; g[a].init = true; }
                            }
                            else if (!g[a].init || v > g[a].f64_val) { g[a].f64_val = v; g[a].init = true; }
                        }
                    };
                    if (batch.has_sel)
                    {
                        for (size_t s = 0; s < batch.sel_count; ++s)
                            accum_one(batch.sel[s]);
                    }
                    else
                    {
                        for (size_t i = 0; i < batch.count; ++i)
                            accum_one(i);
                    }
                }
                return true;
            }

            if (mode == GroupMode::TwoChar1)
            {
                const uint8_t * u0 = batch.columns[gk0].u8_data.data();
                const uint8_t * u1 = batch.columns[gk1].u8_data.data();
                if (batch.has_sel)
                {
                    for (size_t s = 0; s < batch.sel_count; ++s)
                    {
                        const size_t i = batch.sel[s];
                        const uint32_t code = (static_cast<uint32_t>(u0[i]) << 8) | u1[i];
                        int32_t gid = char2_table[code];
                        if (gid < 0)
                        {
                            uint32_t new_gid = 0;
                            if (!alloc_group(batch, i, new_gid))
                                return false;
                            char2_table[code] = static_cast<int32_t>(new_gid);
                            gid = static_cast<int32_t>(new_gid);
                        }
                        update_group_scalar(static_cast<uint32_t>(gid), i, batch);
                    }
                }
                else
                {
                    for (size_t i = 0; i < batch.count; ++i)
                    {
                        const uint32_t code = (static_cast<uint32_t>(u0[i]) << 8) | u1[i];
                        int32_t gid = char2_table[code];
                        if (gid < 0)
                        {
                            uint32_t new_gid = 0;
                            if (!alloc_group(batch, i, new_gid))
                                return false;
                            char2_table[code] = static_cast<int32_t>(new_gid);
                            gid = static_cast<int32_t>(new_gid);
                        }
                        update_group_scalar(static_cast<uint32_t>(gid), i, batch);
                    }
                }
                return true;
            }

            if (mode == GroupMode::OneI64)
            {
                const auto & c0 = batch.columns[gk0];
                if (c0.has_nulls)
                {
                    aborted_fallback = true;
                    return false;
                }
                const int64_t * k0 = c0.i64_data.data();
                auto handle_row = [&](size_t i) -> bool
                {
                    const int64_t key = k0[i];
                    const uint32_t * found = map_i64.find(key);
                    uint32_t gid = 0;
                    if (found)
                    {
                        gid = *found;
                    }
                    else
                    {
                        if (!alloc_group(batch, i, gid))
                            return false;
                        bool ins = false;
                        map_i64.get_or_insert(key, ins) = gid;
                    }
                    update_group_scalar(gid, i, batch);
                    return true;
                };
                if (batch.has_sel)
                {
                    for (size_t s = 0; s < batch.sel_count; ++s)
                        if (!handle_row(batch.sel[s]))
                            return false;
                }
                else
                {
                    for (size_t i = 0; i < batch.count; ++i)
                        if (!handle_row(i))
                            return false;
                }
                return true;
            }

            // GroupMode::TwoI64
            {
                const auto & c0 = batch.columns[gk0];
                const auto & c1 = batch.columns[gk1];
                if (c0.has_nulls || c1.has_nulls)
                {
                    aborted_fallback = true;
                    return false;
                }
                const int64_t * k0 = c0.i64_data.data();
                const int64_t * k1 = c1.i64_data.data();
                auto handle_row = [&](size_t i) -> bool
                {
                    const uint32_t * found = map_pair64.find(k0[i], k1[i]);
                    uint32_t gid = 0;
                    if (found)
                    {
                        gid = *found;
                    }
                    else
                    {
                        if (!alloc_group(batch, i, gid))
                            return false;
                        bool ins = false;
                        map_pair64.get_or_insert(k0[i], k1[i], ins) = gid;
                    }
                    update_group_scalar(gid, i, batch);
                    return true;
                };
                if (batch.has_sel)
                {
                    for (size_t s = 0; s < batch.sel_count; ++s)
                        if (!handle_row(batch.sel[s]))
                            return false;
                }
                else
                {
                    for (size_t i = 0; i < batch.count; ++i)
                        if (!handle_row(i))
                            return false;
                }
                return true;
            }
        });

    if (aborted_fallback)
        return std::optional<Batch>{};
    if (!streamed)
        return std::unexpected(streamed.error());

    Batch out;
    out.schemas = streamed->schemas;

    int fast_having_agg = -1;
    BinaryOpType fast_having_op = BinaryOpType::Gt;
    double fast_having_rhs = 0.0;
    if (having_cond && having_cond->type == ExprType::BinaryOp)
    {
        const auto & hb = static_cast<const popo::parser::BinaryOpExpr &>(*having_cond);
        if (is_cmp_op(hb.op) && hb.left && hb.right)
        {
            auto match_agg = [&](const ExprPtr & ae, const ExprPtr & le, BinaryOpType op)
            {
                if (le->type != ExprType::Literal)
                    return;
                const auto & lv = static_cast<const popo::parser::LiteralExpr &>(*le).value;
                if (is_null(lv) || !lv.is_numeric())
                    return;
                for (size_t a = 0; a < n_aggs; ++a)
                {
                    if (node.agg_exprs[a].get() == ae.get())
                    {
                        fast_having_agg = static_cast<int>(a);
                        fast_having_op = op;
                        fast_having_rhs = lv.to_f64();
                        return;
                    }
                }
            };
            match_agg(hb.left, hb.right, hb.op);
            if (fast_having_agg < 0)
                match_agg(hb.right, hb.left, flip_cmp_op(hb.op));
        }
    }

    auto compute_agg_val = [&](const GroupAccum & ga, const CompiledAgg & ca) -> Value
    {
        switch (ca.func)
        {
            case popo::parser::AggFuncType::Count:
                return Value(ValueType::Int64, ga.count);
            case popo::parser::AggFuncType::Avg:
                if (ga.count > 0)
                {
                    const double sum = (ca.is_int_result && ca.direct_col >= 0
                        && col_kind_for(col_types[static_cast<size_t>(ca.direct_col)]) == ColKind::Int64)
                        ? static_cast<double>(ga.i64_val)
                        : ga.f64_val;
                    return Value(ValueType::Float64, sum / static_cast<double>(ga.count));
                }
                return Value();
            case popo::parser::AggFuncType::Sum:
            case popo::parser::AggFuncType::Min:
            case popo::parser::AggFuncType::Max:
                if (ga.init)
                {
                    if (ca.is_int_result && ca.direct_col >= 0
                        && col_kind_for(col_types[static_cast<size_t>(ca.direct_col)]) == ColKind::Int64)
                        return Value(ValueType::Int64, ga.i64_val);
                    return Value(ValueType::Float64, ga.f64_val);
                }
                return Value();
            default:
                return Value();
        }
    };

    auto finalize_group = [&](size_t gid, Row rep) -> popo::base::Result<void>
    {
        const GroupAccum * g = accums.data() + gid * n_aggs;
        if (fast_having_agg >= 0)
        {
            const size_t ha = static_cast<size_t>(fast_having_agg);
            Value hv = compute_agg_val(g[ha], aggs[ha]);
            if (is_null(hv))
                return {};
            const double lhs = hv.to_f64();
            bool pass = false;
            switch (fast_having_op)
            {
                case BinaryOpType::Eq: pass = (lhs == fast_having_rhs); break;
                case BinaryOpType::Ne: pass = (lhs != fast_having_rhs); break;
                case BinaryOpType::Lt: pass = (lhs < fast_having_rhs); break;
                case BinaryOpType::Le: pass = (lhs <= fast_having_rhs); break;
                case BinaryOpType::Gt: pass = (lhs > fast_having_rhs); break;
                case BinaryOpType::Ge: pass = (lhs >= fast_having_rhs); break;
                default: break;
            }
            if (!pass)
                return {};
        }

        std::unordered_map<const Expr *, Value> agg_values;
        agg_values.reserve(n_aggs);
        for (size_t a = 0; a < n_aggs; ++a)
            agg_values.emplace(node.agg_exprs[a].get(), compute_agg_val(g[a], aggs[a]));

        if (having_cond && fast_having_agg < 0)
        {
            EvalEnv henv{&out, &rep, outer, &agg_values, outer ? outer->depth + 1 : 0};
            POPO_ASSIGN_OR_RETURN(Value cond, eval(having_cond, henv));
            if (!as_bool(cond))
                return {};
        }

        out.rows.push_back(std::move(rep));
        out.row_aggs.push_back(std::move(agg_values));
        return {};
    };

    if (mode == GroupMode::Global)
    {
        Row rep;
        const size_t w = out.width();
        rep.reserve_values(w);
        for (size_t i = 0; i < w; ++i)
            rep.append_value(Value());
        POPO_TRY(finalize_group(0, std::move(rep)));
    }
    else
    {
        if (!having_cond)
        {
            out.rows.reserve(group_reps.size());
            out.row_aggs.reserve(group_reps.size());
        }
        for (size_t gid = 0; gid < group_reps.size(); ++gid)
            POPO_TRY(finalize_group(gid, std::move(group_reps[gid])));
    }
    return std::optional<Batch>(std::move(out));
}

} // namespace popo::executor
