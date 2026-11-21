#include "executor/executor.h"

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/vector/aligned_buffer.h"
#include "executor/vector/simd_kernels.h"
#include "executor/vector/vector_constants.h"
#include "executor/vector/vector_pred.h"
#include "function/date_functions.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::agg_display_name;
using popo::function::as_bool;
using popo::function::eq_ci;
using popo::function::is_floating;
using popo::function::is_null;
using popo::function::pack_str10;
using popo::function::to_i64;
using popo::parser::BinaryOpType;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<Batch> Executor::stream_scan_filter_vectorized(
    const popo::planner::ScanNode & scan, const ExprPtr & filter_cond,
    const EvalEnv * outer,
    const std::function<popo::base::Result<bool>(ColumnarBatch &, const Batch &)> & batch_consumer)
{
    auto qr = schema_mgr_.query_table(scan.table_name);
    if (!qr || !*qr)
        return executor_error(
            std::format("Table '{}' not found", scan.table_name),
            popo::base::Errno::TableNotFound);
    const auto & schema = *qr;
    const size_t total_phys_cols = schema->columns_.size();

    std::vector<uint16_t> proj_cols;
    if (scan.has_projection)
    {
        proj_cols = scan.projected_columns;
    }
    else
    {
        proj_cols.resize(total_phys_cols);
        for (size_t i = 0; i < total_phys_cols; ++i)
            proj_cols[i] = static_cast<uint16_t>(i);
    }

    Batch layout;
    ColumnSchema cs;
    cs.table = scan.alias.empty() ? scan.table_name : scan.alias;
    cs.alias = scan.alias;
    std::vector<ValueType> col_types;
    col_types.reserve(proj_cols.size());
    cs.columns.reserve(proj_cols.size());
    for (size_t idx : proj_cols)
    {
        if (idx < total_phys_cols)
        {
            const std::string & col_name = (!scan.column_names.empty() && idx < scan.column_names.size())
                ? scan.column_names[idx]
                : schema->columns_[idx].column_name_;
            cs.columns.push_back(col_name);
            col_types.push_back(schema->columns_[idx].data_type_);
        }
    }
    layout.schemas.push_back(std::move(cs));

    std::optional<VectorPred> compiled_pred;
    if (filter_cond)
    {
        compiled_pred = compile_vector_pred(
            filter_cond, layout.schemas[0].table, scan.alias, layout.schemas[0].columns);
    }

    ColumnarBatch batch;
    batch.init(col_types, kVectorBatchSize);

    AlignedBuffer<uint8_t> mask_buf(kVectorBatchSize);
    AlignedBuffer<double> cast_f64_buf(kVectorBatchSize);

    std::function<popo::base::Result<void>(const VectorPred &, uint8_t *, const uint8_t *)> eval_pred;
    eval_pred = [&](const VectorPred & p, uint8_t * out_mask, const uint8_t * active_mask) -> popo::base::Result<void>
    {
        const size_t n = batch.count;
        switch (p.kind)
        {
            case VectorPred::Kind::And: {
                simd_fill_u8(out_mask, n, 0xFF);
                if (active_mask)
                    simd_and_masks(out_mask, active_mask, n);
                AlignedBuffer<uint8_t> tmp(n);
                for (const auto & ch : p.children)
                {
                    if (!mask_has_any_nonzero(out_mask, n))
                        return {};
                    POPO_TRY(eval_pred(ch, tmp.data(), out_mask));
                    simd_and_masks(out_mask, tmp.data(), n);
                }
                return {};
            }
            case VectorPred::Kind::Or: {
                simd_fill_u8(out_mask, n, 0);
                AlignedBuffer<uint8_t> tmp(n);
                for (const auto & ch : p.children)
                {
                    POPO_TRY(eval_pred(ch, tmp.data(), active_mask));
                    simd_or_masks(out_mask, tmp.data(), n);
                }
                if (active_mask)
                    simd_and_masks(out_mask, active_mask, n);
                return {};
            }
            case VectorPred::Kind::IsNull: {
                const auto & col = batch.columns[p.col1];
                if (!col.has_nulls)
                {
                    simd_fill_u8(out_mask, n, p.negated ? 0xFF : 0);
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                        out_mask[i] = (col.nulls[i] != 0) ? (p.negated ? 0 : 0xFF) : (p.negated ? 0xFF : 0);
                }
                return {};
            }
            case VectorPred::Kind::CmpColConst: {
                if (is_null(p.lit1))
                {
                    simd_fill_u8(out_mask, n, 0);
                    return {};
                }
                const auto & col = batch.columns[p.col1];
                const auto cmp = p.op;
                if (col.kind == ColKind::Int64 && p.lit1.is_numeric())
                {
                    if (is_floating(p.lit1))
                    {
                        simd_cast_i64_to_f64(col.i64_data.data(), n, cast_f64_buf.data());
                        simd_cmp_f64_const(cast_f64_buf.data(), n, p.lit1.to_f64(), cmp, out_mask);
                    }
                    else
                    {
                        simd_cmp_i64_const(col.i64_data.data(), n, to_i64(p.lit1), cmp, out_mask);
                    }
                }
                else if (col.kind == ColKind::Float64 && p.lit1.is_numeric())
                {
                    simd_cmp_f64_const(col.f64_data.data(), n, p.lit1.to_f64(), cmp, out_mask);
                }
                else if (col.kind == ColKind::Bool && p.lit1.datatype() == ValueType::Bool)
                {
                    simd_cmp_u8_const(col.u8_data.data(), n, p.lit1.boolean() ? 1 : 0, cmp, out_mask);
                }
                else if (col.kind == ColKind::String && p.lit1.datatype() == ValueType::String)
                {
                    const auto & s = p.lit1.string_ref();
                    if (col.all_len1 && s.size() == 1)
                    {
                        simd_cmp_u8_const(col.u8_data.data(), n, static_cast<uint8_t>(s[0]), cmp, out_mask);
                    }
                    else if (col.all_len10 && p.lit1_is_str10)
                    {
                        simd_cmp_str10_const(
                            col.str10_hi.data(), col.str10_lo.data(), n,
                            p.str10_hi, p.str10_lo, cmp, out_mask);
                    }
                    else
                    {
                        const std::string_view rhs(s);
                        for (size_t i = 0; i < n; ++i)
                        {
                            const std::string_view lhs = col.str_at(i);
                            bool r = false;
                            switch (cmp)
                            {
                                case BinaryOpType::Eq: r = (lhs == rhs); break;
                                case BinaryOpType::Ne: r = (lhs != rhs); break;
                                case BinaryOpType::Lt: r = (lhs < rhs); break;
                                case BinaryOpType::Le: r = (lhs <= rhs); break;
                                case BinaryOpType::Gt: r = (lhs > rhs); break;
                                case BinaryOpType::Ge: r = (lhs >= rhs); break;
                                default: break;
                            }
                            out_mask[i] = r ? 0xFF : 0;
                        }
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (active_mask && !active_mask[i])
                        {
                            out_mask[i] = 0;
                            continue;
                        }
                        Row row = batch.extract_row(i);
                        EvalEnv env{&layout, &row, outer, nullptr, outer ? outer->depth + 1 : 0};
                        POPO_ASSIGN_OR_RETURN(Value v, eval(p.fallback_expr, env));
                        out_mask[i] = as_bool(v) ? 0xFF : 0;
                    }
                    return {};
                }
                if (col.has_nulls)
                    simd_clear_nulls(out_mask, col.nulls.data(), n);
                return {};
            }
            case VectorPred::Kind::CmpColCol: {
                const auto & c1 = batch.columns[p.col1];
                const auto & c2 = batch.columns[p.col2];
                const auto cmp = p.op;
                if (c1.kind == ColKind::Int64 && c2.kind == ColKind::Int64)
                {
                    simd_cmp_i64_col(c1.i64_data.data(), c2.i64_data.data(), n, cmp, out_mask);
                }
                else if (c1.kind == ColKind::Float64 && c2.kind == ColKind::Float64)
                {
                    simd_cmp_f64_col(c1.f64_data.data(), c2.f64_data.data(), n, cmp, out_mask);
                }
                else if (c1.kind == ColKind::Int64 && c2.kind == ColKind::Float64)
                {
                    simd_cast_i64_to_f64(c1.i64_data.data(), n, cast_f64_buf.data());
                    simd_cmp_f64_col(cast_f64_buf.data(), c2.f64_data.data(), n, cmp, out_mask);
                }
                else if (c1.kind == ColKind::Float64 && c2.kind == ColKind::Int64)
                {
                    simd_cast_i64_to_f64(c2.i64_data.data(), n, cast_f64_buf.data());
                    simd_cmp_f64_col(c1.f64_data.data(), cast_f64_buf.data(), n, cmp, out_mask);
                }
                else if (c1.kind == ColKind::String && c2.kind == ColKind::String)
                {
                    if (c1.all_len10 && c2.all_len10)
                    {
                        simd_cmp_str10_col(
                            c1.str10_hi.data(), c1.str10_lo.data(),
                            c2.str10_hi.data(), c2.str10_lo.data(),
                            n, cmp, out_mask);
                    }
                    else
                    {
                        for (size_t i = 0; i < n; ++i)
                        {
                            const std::string_view l = c1.str_at(i);
                            const std::string_view r = c2.str_at(i);
                            bool res = false;
                            switch (cmp)
                            {
                                case BinaryOpType::Eq: res = (l == r); break;
                                case BinaryOpType::Ne: res = (l != r); break;
                                case BinaryOpType::Lt: res = (l < r); break;
                                case BinaryOpType::Le: res = (l <= r); break;
                                case BinaryOpType::Gt: res = (l > r); break;
                                case BinaryOpType::Ge: res = (l >= r); break;
                                default: break;
                            }
                            out_mask[i] = res ? 0xFF : 0;
                        }
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (active_mask && !active_mask[i])
                        {
                            out_mask[i] = 0;
                            continue;
                        }
                        Row row = batch.extract_row(i);
                        EvalEnv env{&layout, &row, outer, nullptr, outer ? outer->depth + 1 : 0};
                        POPO_ASSIGN_OR_RETURN(Value v, eval(p.fallback_expr, env));
                        out_mask[i] = as_bool(v) ? 0xFF : 0;
                    }
                    return {};
                }
                if (c1.has_nulls)
                    simd_clear_nulls(out_mask, c1.nulls.data(), n);
                if (c2.has_nulls)
                    simd_clear_nulls(out_mask, c2.nulls.data(), n);
                return {};
            }
            case VectorPred::Kind::BetweenConst: {
                if (is_null(p.lit1) || is_null(p.lit2))
                {
                    simd_fill_u8(out_mask, n, 0);
                    return {};
                }
                const auto & col = batch.columns[p.col1];
                if (col.kind == ColKind::Float64 && p.lit1.is_numeric() && p.lit2.is_numeric())
                {
                    simd_between_f64(col.f64_data.data(), n, p.lit1.to_f64(), p.lit2.to_f64(), out_mask);
                }
                else if (col.kind == ColKind::Int64 && p.lit1.is_numeric() && p.lit2.is_numeric())
                {
                    if (is_floating(p.lit1) || is_floating(p.lit2))
                    {
                        simd_cast_i64_to_f64(col.i64_data.data(), n, cast_f64_buf.data());
                        simd_between_f64(cast_f64_buf.data(), n, p.lit1.to_f64(), p.lit2.to_f64(), out_mask);
                    }
                    else
                    {
                        simd_between_i64(col.i64_data.data(), n, to_i64(p.lit1), to_i64(p.lit2), out_mask);
                    }
                }
                else if (col.kind == ColKind::String && p.lit1.datatype() == ValueType::String && p.lit2.datatype() == ValueType::String)
                {
                    const auto & s1 = p.lit1.string_ref();
                    const auto & s2 = p.lit2.string_ref();
                    if (col.all_len10 && s1.size() == 10 && s2.size() == 10)
                    {
                        uint64_t hi1 = 0, hi2 = 0;
                        uint16_t lo1 = 0, lo2 = 0;
                        pack_str10(s1.data(), hi1, lo1);
                        pack_str10(s2.data(), hi2, lo2);
                        AlignedBuffer<uint8_t> tmp(n);
                        simd_cmp_str10_const(col.str10_hi.data(), col.str10_lo.data(), n, hi1, lo1, BinaryOpType::Ge, out_mask);
                        simd_cmp_str10_const(col.str10_hi.data(), col.str10_lo.data(), n, hi2, lo2, BinaryOpType::Le, tmp.data());
                        simd_and_masks(out_mask, tmp.data(), n);
                    }
                    else
                    {
                        const std::string_view lo(s1), hi(s2);
                        for (size_t i = 0; i < n; ++i)
                        {
                            const std::string_view v = col.str_at(i);
                            out_mask[i] = (v >= lo && v <= hi) ? 0xFF : 0;
                        }
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (active_mask && !active_mask[i])
                        {
                            out_mask[i] = 0;
                            continue;
                        }
                        Row row = batch.extract_row(i);
                        EvalEnv env{&layout, &row, outer, nullptr, outer ? outer->depth + 1 : 0};
                        POPO_ASSIGN_OR_RETURN(Value v, eval(p.fallback_expr, env));
                        out_mask[i] = as_bool(v) ? 0xFF : 0;
                    }
                    return {};
                }
                if (p.negated)
                    simd_not_mask(out_mask, n);
                if (col.has_nulls)
                    simd_clear_nulls(out_mask, col.nulls.data(), n);
                return {};
            }
            case VectorPred::Kind::InConst: {
                const auto & col = batch.columns[p.col1];
                simd_fill_u8(out_mask, n, 0);
                if (col.kind == ColKind::Int64 && !p.in_i64.empty())
                {
                    AlignedBuffer<uint8_t> tmp(n);
                    for (int64_t val : p.in_i64)
                    {
                        simd_cmp_i64_const(col.i64_data.data(), n, val, BinaryOpType::Eq, tmp.data());
                        simd_or_masks(out_mask, tmp.data(), n);
                    }
                }
                else if (col.kind == ColKind::Float64 && !p.in_f64.empty())
                {
                    AlignedBuffer<uint8_t> tmp(n);
                    for (double val : p.in_f64)
                    {
                        simd_cmp_f64_const(col.f64_data.data(), n, val, BinaryOpType::Eq, tmp.data());
                        simd_or_masks(out_mask, tmp.data(), n);
                    }
                }
                else if (col.kind == ColKind::String && !p.in_str.empty())
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (active_mask && !active_mask[i])
                            continue;
                        const std::string_view v = col.str_at(i);
                        bool hit = false;
                        for (const auto & s : p.in_str)
                        {
                            if (v == s)
                            {
                                hit = true;
                                break;
                            }
                        }
                        out_mask[i] = hit ? 0xFF : 0;
                    }
                }
                else
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (active_mask && !active_mask[i])
                        {
                            out_mask[i] = 0;
                            continue;
                        }
                        Row row = batch.extract_row(i);
                        EvalEnv env{&layout, &row, outer, nullptr, outer ? outer->depth + 1 : 0};
                        POPO_ASSIGN_OR_RETURN(Value v, eval(p.fallback_expr, env));
                        out_mask[i] = as_bool(v) ? 0xFF : 0;
                    }
                    return {};
                }
                if (p.negated)
                    simd_not_mask(out_mask, n);
                if (col.has_nulls)
                    simd_clear_nulls(out_mask, col.nulls.data(), n);
                return {};
            }
            case VectorPred::Kind::LikeConst: {
                const auto & col = batch.columns[p.col1];
                if (col.kind == ColKind::String)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        if ((active_mask && !active_mask[i]) || (col.has_nulls && col.nulls[i]))
                        {
                            out_mask[i] = 0;
                            continue;
                        }
                        const bool m = p.fast_like.match(col.str_at(i));
                        out_mask[i] = (p.negated ? !m : m) ? 0xFF : 0;
                    }
                    return {};
                }
                [[fallthrough]];
            }
            case VectorPred::Kind::Not:
            case VectorPred::Kind::FallbackScalar: {
                for (size_t i = 0; i < n; ++i)
                {
                    if (active_mask && !active_mask[i])
                    {
                        out_mask[i] = 0;
                        continue;
                    }
                    Row row = batch.extract_row(i);
                    EvalEnv env{&layout, &row, outer, nullptr, outer ? outer->depth + 1 : 0};
                    POPO_ASSIGN_OR_RETURN(Value v, eval(p.fallback_expr, env));
                    out_mask[i] = as_bool(v) ? 0xFF : 0;
                }
                return {};
            }
        }
        return {};
    };

    auto process_batch = [&]() -> popo::base::Result<bool>
    {
        if (batch.count == 0)
            return true;
        if (compiled_pred)
        {
            POPO_TRY(eval_pred(*compiled_pred, mask_buf.data(), nullptr));
            batch.sel_count = simd_mask_to_sel(mask_buf.data(), batch.count, batch.sel.data());
            batch.has_sel = true;
            if (batch.sel_count == 0)
                return true;
        }
        else
        {
            batch.has_sel = false;
            batch.sel_count = batch.count;
        }
        return batch_consumer(batch, layout);
    };

    auto cursor = engine_.mvcc()->scan_cursor(txn_, scan.table_name);
    if (cursor.is_clean_baseline())
    {
        for (;;)
        {
            batch.reset();
            const size_t fetched = cursor.next_batch_raw(
                kVectorBatchSize,
                [&](const char * data, uint32_t len)
                {
                    batch.append_serialized_tuple(data, len, *schema, proj_cols);
                });
            if (fetched == 0)
                break;
            POPO_ASSIGN_OR_RETURN(bool keep_going, process_batch());
            if (!keep_going)
                break;
        }
    }
    else
    {
        Row row;
        for (;;)
        {
            batch.reset();
            while (batch.count < kVectorBatchSize && cursor.next(row))
            {
                if (scan.has_projection)
                {
                    Row proj_row;
                    proj_row.reserve_values(proj_cols.size());
                    for (size_t idx : proj_cols)
                        proj_row.append_value(idx < row.values_.size() ? std::move(row.values_[idx]) : Value());
                    batch.append_row(proj_row);
                }
                else
                {
                    batch.append_row(row);
                }
                row = Row{};
            }
            if (batch.count == 0)
                break;
            POPO_ASSIGN_OR_RETURN(bool keep_going, process_batch());
            if (!keep_going)
                break;
        }
    }
    return layout;
}

popo::base::Result<Batch> Executor::exec_scan(const popo::planner::ScanNode & node)
{
    Batch batch;
    auto streamed = stream_scan_filter_vectorized(
        node, nullptr, nullptr,
        [&](ColumnarBatch & cb, const Batch & layout) -> popo::base::Result<bool>
        {
            if (batch.schemas.empty())
                batch.schemas = layout.schemas;
            batch.rows.reserve(batch.rows.size() + cb.count);
            for (size_t i = 0; i < cb.count; ++i)
                batch.rows.push_back(cb.extract_row(i));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    if (batch.schemas.empty())
        batch.schemas = streamed->schemas;
    return batch;
}

popo::base::Result<Batch>
Executor::exec_subquery_scan(const popo::planner::SubqueryScanNode & node, const EvalEnv * outer)
{
    auto it = materialized_.find(node.child.get());
    if (it != materialized_.end())
    {
        Batch batch;
        ColumnSchema cs;
        cs.table = node.alias;
        cs.alias = node.alias;
        cs.columns = node.column_names.empty() ? it->second.flat_names() : node.column_names;
        batch.schemas.push_back(std::move(cs));
        batch.rows = it->second.rows;
        return batch;
    }

    if (node.recursive || !node.cte_name.empty() || outer == nullptr)
    {
        auto child_plan = (!node.recursive && !node.cte_name.empty()) ? optimize_plan(node.child, false) : node.child;
        popo::base::Result<Batch> built_result =
            node.recursive ? exec_recursive(node) : exec_batch(child_plan, nullptr);
        if (!built_result)
            return std::unexpected(built_result.error());
        Batch built = std::move(*built_result);
        it = materialized_.emplace(node.child.get(), std::move(built)).first;
        Batch batch;
        ColumnSchema cs;
        cs.table = node.alias;
        cs.alias = node.alias;
        cs.columns = node.column_names.empty() ? it->second.flat_names() : node.column_names;
        batch.schemas.push_back(std::move(cs));
        batch.rows = it->second.rows;
        return batch;
    }

    const int saved = min_env_depth_seen_;
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    POPO_ASSIGN_OR_RETURN(Batch built, exec_batch(node.child, outer));
    const bool correlated = min_env_depth_seen_ <= outer->depth;
    min_env_depth_seen_ = std::min(saved, min_env_depth_seen_);

    Batch batch;
    ColumnSchema cs;
    cs.table = node.alias;
    cs.alias = node.alias;
    cs.columns = node.column_names.empty() ? built.flat_names() : node.column_names;
    batch.schemas.push_back(std::move(cs));
    if (!correlated)
    {
        it = materialized_.emplace(node.child.get(), std::move(built)).first;
        batch.rows = it->second.rows;
    }
    else
    {
        batch.rows = std::move(built.rows);
    }
    return batch;
}

popo::base::Result<Batch>
Executor::exec_filter(const popo::planner::FilterNode & node, const EvalEnv * outer)
{
    const popo::planner::ScanNode * scan = nullptr;
    ExprPtr child_filter;
    if (extract_scan_and_filters(node.child, scan, child_filter))
    {
        ExprPtr combined = child_filter ? combine_and(child_filter, node.condition) : node.condition;
        Batch out;
        auto streamed = stream_scan_filter_vectorized(
            *scan, combined, outer,
            [&](ColumnarBatch & cb, const Batch & layout) -> popo::base::Result<bool>
            {
                if (out.schemas.empty())
                    out.schemas = layout.schemas;
                if (cb.has_sel)
                {
                    out.rows.reserve(out.rows.size() + cb.sel_count);
                    for (size_t s = 0; s < cb.sel_count; ++s)
                        out.rows.push_back(cb.extract_row(cb.sel[s]));
                }
                else
                {
                    out.rows.reserve(out.rows.size() + cb.count);
                    for (size_t i = 0; i < cb.count; ++i)
                        out.rows.push_back(cb.extract_row(i));
                }
                return true;
            });
        if (!streamed)
            return std::unexpected(streamed.error());
        if (out.schemas.empty())
            out.schemas = streamed->schemas;
        return out;
    }
    if (node.child && node.child->type == popo::planner::PlanNodeType::Agg)
    {
        POPO_ASSIGN_OR_RETURN(
            auto vec_agg,
            try_exec_vectorized_agg(
                static_cast<const popo::planner::AggNode &>(*node.child),
                outer, node.condition));
        if (vec_agg)
            return std::move(*vec_agg);
    }
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    out.schemas = batch.schemas;
    const bool has_aggs = !batch.row_aggs.empty();
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        env.row = &batch.rows[i];
        env.aggs = has_aggs ? &batch.row_aggs[i] : nullptr;
        POPO_ASSIGN_OR_RETURN(auto condition, eval(node.condition, env));
        if (as_bool(condition))
        {
            out.rows.push_back(std::move(batch.rows[i]));
            if (has_aggs)
                out.row_aggs.push_back(std::move(batch.row_aggs[i]));
        }
    }
    return out;
}

popo::base::Result<Batch>
Executor::exec_project(const popo::planner::ProjectNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    ColumnSchema cs;
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;

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
                const bool include = star.table_name.empty() || eq_ci(star.table_name, s.table)
                    || (!s.alias.empty() && eq_ci(star.table_name, s.alias));
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
                name = agg_display_name(static_cast<const popo::parser::FunctionCallExpr &>(*expr));
            else
                name = std::format("column{}", cs.columns.size());
        }
        cs.columns.push_back(name);
        outputs.push_back(expr);
    }
    out.schemas.push_back(std::move(cs));

    const bool has_aggs = !batch.row_aggs.empty();
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
                projected.append_value(row.values_[star_columns[star_idx++]]);
            else
            {
                POPO_ASSIGN_OR_RETURN(auto value, eval(expr, env));
                projected.append_value(std::move(value));
            }
        }
        out.rows.push_back(std::move(projected));
    }
    return out;
}

} // namespace popo::executor
