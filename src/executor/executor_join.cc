#include "executor/executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/memory_charge_guard.h"
#include "executor/spill/spill_algorithms.h"
#include "executor/spill/spill_codec.h"
#include "executor/spill/spill_reader.h"
#include "executor/spill/spill_writer.h"
#include "executor/types/join_input.h"
#include "executor/vector/aligned_buffer.h"
#include "executor/vector/flat_int64_multi_map.h"
#include "executor/vector/flat_pair64_multi_map.h"
#include "executor/vector/simd_kernels.h"
#include "executor/vector/vector_constants.h"
#include "executor/vector/vector_pred.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::as_bool;
using popo::function::eq_ci;
using popo::function::is_integer_value;
using popo::function::is_null;
using popo::function::to_i64;
using popo::function::value_key;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<Batch> Executor::stream_join(
    const popo::planner::JoinNode & node, const EvalEnv * outer,
    const StreamConsumer & consumer)
{
    const bool is_inner = node.join_type == popo::parser::JoinType::Inner;
    const bool is_left = node.join_type == popo::parser::JoinType::Left;
    const bool spillable = is_inner || is_left;
    const bool build_on_left = is_inner && (estimate_plan_rows(node.left) < estimate_plan_rows(node.right));
    const auto & build_node = build_on_left ? node.left : node.right;
    const auto & probe_node = build_on_left ? node.right : node.left;

    Batch build_batch;
    size_t build_charged = 0;
    bool build_overflow = false;
    const size_t build_soft_limit = (memory_ && spillable)
        ? std::max<size_t>(memory_->limit() / 2, 4096)
        : std::numeric_limits<size_t>::max();
    {
        auto streamed_build = stream_rows(
            build_node, outer,
            [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
            {
                if (build_batch.schemas.empty())
                    build_batch.schemas = layout.schemas;
                const size_t bytes = estimate_row_bytes(item.row) + 48;
                if (spillable && memory_)
                {
                    if (build_charged + bytes > build_soft_limit || !memory_->try_reserve(bytes))
                    {
                        build_overflow = true;
                        return false;
                    }
                    build_charged += bytes;
                }
                build_batch.rows.push_back(std::move(item.row));
                return true;
            });
        if (!streamed_build && !build_overflow)
        {
            if (memory_ && build_charged > 0)
                memory_->release(build_charged);
            return std::unexpected(streamed_build.error());
        }
        if (streamed_build && build_batch.schemas.empty())
            build_batch.schemas = streamed_build->schemas;
    }

    if (build_overflow)
    {
        if (memory_ && build_charged > 0)
            memory_->release(build_charged);
        build_batch.rows.clear();
        POPO_ASSIGN_OR_RETURN(Batch joined, exec_join(node, outer));
        Batch layout;
        layout.schemas = joined.schemas;
        if (joined.spilled_rows)
        {
            SpillReader reader(*joined.spilled_rows);
            for (;;)
            {
                auto row = reader.next();
                if (!row)
                    return std::unexpected(row.error());
                if (!*row)
                    break;
                POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(StreamItem{std::move(**row), {}}, layout));
                if (!keep_going)
                    break;
            }
        }
        else
        {
            for (auto & row : joined.rows)
            {
                POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(StreamItem{std::move(row), {}}, layout));
                if (!keep_going)
                    break;
            }
        }
        return layout;
    }

    MemoryChargeGuard build_guard(memory_.get());
    build_guard.adopt(build_charged);

    const bool emit_unmatched_probe = (!build_on_left)
        && (node.join_type == popo::parser::JoinType::Left || node.join_type == popo::parser::JoinType::Full);
    const bool emit_unmatched_build = (!build_on_left)
        && (node.join_type == popo::parser::JoinType::Right || node.join_type == popo::parser::JoinType::Full);
    std::vector<uint8_t> build_matched(emit_unmatched_build ? build_batch.rows.size() : 0, 0);

    Row null_build;
    const size_t build_width = build_batch.width();
    null_build.reserve_values(build_width);
    for (size_t i = 0; i < build_width; ++i)
        null_build.append_value(Value());

    auto concat_lr = [build_on_left](const Row & probe_r, const Row & build_r)
    {
        const Row & l = build_on_left ? build_r : probe_r;
        const Row & r = build_on_left ? probe_r : build_r;
        Row out;
        out.reserve_values(l.values_.size() + r.values_.size());
        out.values_ = l.values_;
        out.values_.insert(out.values_.end(), r.values_.begin(), r.values_.end());
        return out;
    };

    Batch out_layout;
    std::vector<std::pair<ExprPtr, ExprPtr>> equi_keys;
    ExprPtr residual_cond;
    bool hash_built = false;

    enum class HashMode : uint8_t { None, FlatI64, FlatPair64, StringMap };
    HashMode hash_mode = HashMode::None;
    FlatInt64MultiMap flat_i64;
    FlatPair64MultiMap flat_pair64;
    std::unordered_map<std::string, std::vector<uint32_t>> str_map;

    EvalEnv res_env;
    res_env.layout = &out_layout;
    res_env.outer = outer;
    res_env.depth = outer ? outer->depth + 1 : 0;

    auto init_hash = [&](const Batch & probe_layout) -> popo::base::Result<void>
    {
        if (hash_built)
            return {};
        hash_built = true;
        const auto & left_schemas = build_on_left ? build_batch.schemas : probe_layout.schemas;
        const auto & right_schemas = build_on_left ? probe_layout.schemas : build_batch.schemas;
        out_layout.schemas = left_schemas;
        out_layout.schemas.insert(out_layout.schemas.end(), right_schemas.begin(), right_schemas.end());
        split_join_equi_and_residual(node.condition, left_schemas, right_schemas, equi_keys, residual_cond);

        if (equi_keys.empty() || build_batch.rows.empty())
            return {};

        EvalEnv build_env{&build_batch, nullptr, outer, nullptr, res_env.depth};

        if (equi_keys.size() == 1)
        {
            const ExprPtr & bkey_expr = build_on_left ? equi_keys[0].first : equi_keys[0].second;
            const size_t n = build_batch.rows.size();
            std::vector<int64_t> bkeys(n, 0);
            std::vector<uint8_t> bnull(n, 0);
            bool all_int = true;
            for (size_t i = 0; i < n; ++i)
            {
                build_env.row = &build_batch.rows[i];
                POPO_ASSIGN_OR_RETURN(Value v, eval(bkey_expr, build_env));
                if (is_null(v))
                {
                    bnull[i] = 1;
                    continue;
                }
                if (!is_integer_value(v))
                {
                    all_int = false;
                    break;
                }
                bkeys[i] = to_i64(v);
            }
            if (all_int)
            {
                flat_i64.build(bkeys, bnull);
                hash_mode = HashMode::FlatI64;
                return {};
            }
        }
        else if (equi_keys.size() == 2)
        {
            const ExprPtr & bkey0 = build_on_left ? equi_keys[0].first : equi_keys[0].second;
            const ExprPtr & bkey1 = build_on_left ? equi_keys[1].first : equi_keys[1].second;
            const size_t n = build_batch.rows.size();
            std::vector<int64_t> bk0(n, 0);
            std::vector<int64_t> bk1(n, 0);
            std::vector<uint8_t> bnull(n, 0);
            bool all_int = true;
            for (size_t i = 0; i < n; ++i)
            {
                build_env.row = &build_batch.rows[i];
                POPO_ASSIGN_OR_RETURN(Value v0, eval(bkey0, build_env));
                if (is_null(v0))
                {
                    bnull[i] = 1;
                    continue;
                }
                POPO_ASSIGN_OR_RETURN(Value v1, eval(bkey1, build_env));
                if (is_null(v1))
                {
                    bnull[i] = 1;
                    continue;
                }
                if (!is_integer_value(v0) || !is_integer_value(v1))
                {
                    all_int = false;
                    break;
                }
                bk0[i] = to_i64(v0);
                bk1[i] = to_i64(v1);
            }
            if (all_int)
            {
                flat_pair64.build(bk0, bk1, bnull);
                hash_mode = HashMode::FlatPair64;
                return {};
            }
        }

        str_map.reserve(build_batch.rows.size());
        for (size_t i = 0; i < build_batch.rows.size(); ++i)
        {
            build_env.row = &build_batch.rows[i];
            std::string key;
            bool any_null = false;
            for (const auto & [lexpr, rexpr] : equi_keys)
            {
                POPO_ASSIGN_OR_RETURN(Value v, eval(build_on_left ? lexpr : rexpr, build_env));
                if (is_null(v))
                {
                    any_null = true;
                    break;
                }
                key += value_key(v);
                key.push_back('\x1f');
            }
            if (!any_null)
                str_map[std::move(key)].push_back(static_cast<uint32_t>(i));
        }
        hash_mode = HashMode::StringMap;
        return {};
    };

    auto emit_pair = [&](const Row & probe_row, uint32_t bidx, bool & matched_any) -> popo::base::Result<bool>
    {
        Row joined = concat_lr(probe_row, build_batch.rows[bidx]);
        if (residual_cond)
        {
            res_env.row = &joined;
            POPO_ASSIGN_OR_RETURN(Value c, eval(residual_cond, res_env));
            if (!as_bool(c))
                return true;
        }
        matched_any = true;
        if (emit_unmatched_build)
            build_matched[bidx] = 1;
        return consumer(StreamItem{std::move(joined), {}}, out_layout);
    };

    const popo::planner::ScanNode * probe_scan = nullptr;
    ExprPtr probe_filter;
    if (extract_scan_and_filters(probe_node, probe_scan, probe_filter))
    {
        auto qr = schema_mgr_.query_table(probe_scan->table_name);
        if (qr && *qr)
        {
            const auto & schema = *qr;
            const size_t total_phys = schema->columns_.size();
            Batch probe_layout;
            ColumnSchema pcs;
            pcs.table = probe_scan->alias.empty() ? probe_scan->table_name : probe_scan->alias;
            pcs.alias = probe_scan->alias;
            std::vector<ValueType> ptypes;
            auto add_col = [&](size_t idx)
            {
                if (idx < total_phys)
                {
                    pcs.columns.push_back(
                        (!probe_scan->column_names.empty() && idx < probe_scan->column_names.size())
                            ? probe_scan->column_names[idx]
                            : schema->columns_[idx].column_name_);
                    ptypes.push_back(schema->columns_[idx].data_type_);
                }
            };
            if (probe_scan->has_projection)
                for (size_t idx : probe_scan->projected_columns)
                    add_col(idx);
            else
                for (size_t idx = 0; idx < total_phys; ++idx)
                    add_col(idx);
            probe_layout.schemas.push_back(std::move(pcs));

            POPO_TRY(init_hash(probe_layout));

            if (is_inner && build_batch.rows.empty())
                return out_layout;

            int pcol0 = -1;
            int pcol1 = -1;
            if (hash_mode == HashMode::FlatI64)
            {
                const ExprPtr & pexpr = build_on_left ? equi_keys[0].second : equi_keys[0].first;
                if (pexpr && pexpr->type == ExprType::ColumnRef)
                {
                    int c = resolve_single_scan_col(
                        probe_layout.schemas[0].table, probe_scan->alias, probe_layout.schemas[0].columns,
                        static_cast<const popo::parser::ColumnRefExpr &>(*pexpr));
                    if (c >= 0 && col_kind_for(ptypes[static_cast<size_t>(c)]) == ColKind::Int64)
                        pcol0 = c;
                }
            }
            else if (hash_mode == HashMode::FlatPair64)
            {
                const ExprPtr & pe0 = build_on_left ? equi_keys[0].second : equi_keys[0].first;
                const ExprPtr & pe1 = build_on_left ? equi_keys[1].second : equi_keys[1].first;
                if (pe0 && pe0->type == ExprType::ColumnRef && pe1 && pe1->type == ExprType::ColumnRef)
                {
                    int c0 = resolve_single_scan_col(
                        probe_layout.schemas[0].table, probe_scan->alias, probe_layout.schemas[0].columns,
                        static_cast<const popo::parser::ColumnRefExpr &>(*pe0));
                    int c1 = resolve_single_scan_col(
                        probe_layout.schemas[0].table, probe_scan->alias, probe_layout.schemas[0].columns,
                        static_cast<const popo::parser::ColumnRefExpr &>(*pe1));
                    if (c0 >= 0 && c1 >= 0
                        && col_kind_for(ptypes[static_cast<size_t>(c0)]) == ColKind::Int64
                        && col_kind_for(ptypes[static_cast<size_t>(c1)]) == ColKind::Int64)
                    {
                        pcol0 = c0;
                        pcol1 = c1;
                    }
                }
            }

            if (pcol0 >= 0)
            {
                AlignedBuffer<uint64_t> hash_buf(kVectorBatchSize);
                AlignedBuffer<int64_t> gathered_keys(kVectorBatchSize);
                auto streamed = stream_scan_filter_vectorized(
                    *probe_scan, probe_filter, outer,
                    [&](ColumnarBatch & cb, const Batch &) -> popo::base::Result<bool>
                    {
                        const size_t active = cb.has_sel ? cb.sel_count : cb.count;
                        if (active == 0)
                            return true;
                        const auto & c0 = cb.columns[static_cast<size_t>(pcol0)];
                        const int64_t * k0 = c0.i64_data.data();

                        if (hash_mode == HashMode::FlatI64)
                        {
                            if (cb.has_sel)
                            {
                                for (size_t s = 0; s < active; ++s)
                                    gathered_keys[s] = k0[cb.sel[s]];
                                simd_hash_i64(gathered_keys.data(), active, hash_buf.data());
                            }
                            else
                            {
                                simd_hash_i64(k0, active, hash_buf.data());
                            }
                            for (size_t s = 0; s < active; ++s)
                            {
                                const size_t r = cb.has_sel ? cb.sel[s] : s;
                                bool matched = false;
                                if (!(c0.has_nulls && c0.nulls[r]))
                                {
                                    uint32_t head = flat_i64.find_head_hashed(k0[r], hash_buf[s]);
                                    if (head != UINT32_MAX)
                                    {
                                        Row prow = cb.extract_row(r);
                                        for (uint32_t bidx = head; bidx != FlatInt64MultiMap::kEmpty; bidx = flat_i64.next_of(bidx))
                                        {
                                            POPO_ASSIGN_OR_RETURN(
                                                bool keep_going,
                                                emit_pair(prow, bidx, matched));
                                            if (!keep_going)
                                                return false;
                                        }
                                        if (!matched && emit_unmatched_probe)
                                        {
                                            POPO_ASSIGN_OR_RETURN(
                                                bool keep_going,
                                                consumer(StreamItem{concat_lr(prow, null_build), {}}, out_layout));
                                            if (!keep_going)
                                                return false;
                                        }
                                        continue;
                                    }
                                }
                                if (emit_unmatched_probe)
                                {
                                    Row prow = cb.extract_row(r);
                                    POPO_ASSIGN_OR_RETURN(
                                        bool keep_going,
                                        consumer(StreamItem{concat_lr(prow, null_build), {}}, out_layout));
                                    if (!keep_going)
                                        return false;
                                }
                            }
                            return true;
                        }

                        const auto & c1 = cb.columns[static_cast<size_t>(pcol1)];
                        const int64_t * k1 = c1.i64_data.data();
                        for (size_t s = 0; s < active; ++s)
                        {
                            const size_t r = cb.has_sel ? cb.sel[s] : s;
                            bool matched = false;
                            if (!(c0.has_nulls && c0.nulls[r]) && !(c1.has_nulls && c1.nulls[r]))
                            {
                                uint32_t head = flat_pair64.find_head(k0[r], k1[r]);
                                if (head != UINT32_MAX)
                                {
                                    Row prow = cb.extract_row(r);
                                    for (uint32_t bidx = head; bidx != FlatPair64MultiMap::kEmpty; bidx = flat_pair64.next_of(bidx))
                                    {
                                        POPO_ASSIGN_OR_RETURN(
                                            bool keep_going,
                                            emit_pair(prow, bidx, matched));
                                        if (!keep_going)
                                            return false;
                                    }
                                    if (!matched && emit_unmatched_probe)
                                    {
                                        POPO_ASSIGN_OR_RETURN(
                                            bool keep_going,
                                            consumer(StreamItem{concat_lr(prow, null_build), {}}, out_layout));
                                        if (!keep_going)
                                            return false;
                                    }
                                    continue;
                                }
                            }
                            if (emit_unmatched_probe)
                            {
                                Row prow = cb.extract_row(r);
                                POPO_ASSIGN_OR_RETURN(
                                    bool keep_going,
                                    consumer(StreamItem{concat_lr(prow, null_build), {}}, out_layout));
                                if (!keep_going)
                                    return false;
                            }
                        }
                        return true;
                    });
                if (!streamed)
                    return std::unexpected(streamed.error());
                if (emit_unmatched_build)
                {
                    Row null_probe;
                    const size_t pw = probe_layout.width();
                    null_probe.reserve_values(pw);
                    for (size_t i = 0; i < pw; ++i)
                        null_probe.append_value(Value());
                    for (size_t j = 0; j < build_batch.rows.size(); ++j)
                    {
                        if (!build_matched[j])
                        {
                            POPO_ASSIGN_OR_RETURN(
                                bool keep_going,
                                consumer(StreamItem{concat_lr(null_probe, build_batch.rows[j]), {}}, out_layout));
                            if (!keep_going)
                                break;
                        }
                    }
                }
                return out_layout;
            }
        }
    }

    auto streamed_probe = stream_rows(
        probe_node, outer,
        [&](StreamItem && item, const Batch & probe_layout) -> popo::base::Result<bool>
        {
            if (!hash_built)
                POPO_TRY(init_hash(probe_layout));
            if (is_inner && build_batch.rows.empty())
                return false;

            const Row & prow = item.row;
            bool matched = false;
            if (!build_batch.rows.empty())
            {
                EvalEnv probe_env{&probe_layout, &prow, outer, nullptr, res_env.depth};
                if (hash_mode == HashMode::FlatI64)
                {
                    const ExprPtr & pexpr = build_on_left ? equi_keys[0].second : equi_keys[0].first;
                    POPO_ASSIGN_OR_RETURN(Value pv, eval(pexpr, probe_env));
                    if (!is_null(pv))
                    {
                        bool ok_i64 = is_integer_value(pv);
                        int64_t pk = 0;
                        if (ok_i64)
                            pk = to_i64(pv);
                        else if (pv.is_numeric())
                        {
                            double d = pv.to_f64();
                            if (std::floor(d) == d && d >= -9e18 && d <= 9e18)
                            {
                                pk = static_cast<int64_t>(d);
                                ok_i64 = true;
                            }
                        }
                        if (ok_i64)
                        {
                            for (uint32_t bidx = flat_i64.find_head(pk);
                                 bidx != FlatInt64MultiMap::kEmpty;
                                 bidx = flat_i64.next_of(bidx))
                            {
                                POPO_ASSIGN_OR_RETURN(bool keep_going, emit_pair(prow, bidx, matched));
                                if (!keep_going)
                                    return false;
                            }
                        }
                    }
                }
                else if (hash_mode == HashMode::FlatPair64)
                {
                    const ExprPtr & pe0 = build_on_left ? equi_keys[0].second : equi_keys[0].first;
                    const ExprPtr & pe1 = build_on_left ? equi_keys[1].second : equi_keys[1].first;
                    POPO_ASSIGN_OR_RETURN(Value pv0, eval(pe0, probe_env));
                    if (!is_null(pv0))
                    {
                        POPO_ASSIGN_OR_RETURN(Value pv1, eval(pe1, probe_env));
                        if (!is_null(pv1) && is_integer_value(pv0) && is_integer_value(pv1))
                        {
                            for (uint32_t bidx = flat_pair64.find_head(to_i64(pv0), to_i64(pv1));
                                 bidx != FlatPair64MultiMap::kEmpty;
                                 bidx = flat_pair64.next_of(bidx))
                            {
                                POPO_ASSIGN_OR_RETURN(bool keep_going, emit_pair(prow, bidx, matched));
                                if (!keep_going)
                                    return false;
                            }
                        }
                    }
                }
                else if (hash_mode == HashMode::StringMap)
                {
                    std::string key;
                    bool any_null = false;
                    for (const auto & [lexpr, rexpr] : equi_keys)
                    {
                        POPO_ASSIGN_OR_RETURN(Value pv, eval(build_on_left ? rexpr : lexpr, probe_env));
                        if (is_null(pv))
                        {
                            any_null = true;
                            break;
                        }
                        key += value_key(pv);
                        key.push_back('\x1f');
                    }
                    if (!any_null)
                    {
                        auto it = str_map.find(key);
                        if (it != str_map.end())
                        {
                            for (uint32_t bidx : it->second)
                            {
                                POPO_ASSIGN_OR_RETURN(bool keep_going, emit_pair(prow, bidx, matched));
                                if (!keep_going)
                                    return false;
                            }
                        }
                    }
                }
                else
                {
                    for (uint32_t j = 0; j < static_cast<uint32_t>(build_batch.rows.size()); ++j)
                    {
                        POPO_ASSIGN_OR_RETURN(bool keep_going, emit_pair(prow, j, matched));
                        if (!keep_going)
                            return false;
                    }
                }
            }
            if (!matched && emit_unmatched_probe)
            {
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going,
                    consumer(StreamItem{concat_lr(prow, null_build), {}}, out_layout));
                if (!keep_going)
                    return false;
            }
            return true;
        });
    if (!streamed_probe)
        return std::unexpected(streamed_probe.error());
    if (!hash_built)
        POPO_TRY(init_hash(*streamed_probe));
    if (emit_unmatched_build)
    {
        Row null_probe;
        const size_t pw = streamed_probe->width();
        null_probe.reserve_values(pw);
        for (size_t i = 0; i < pw; ++i)
            null_probe.append_value(Value());
        for (size_t j = 0; j < build_batch.rows.size(); ++j)
        {
            if (!build_matched[j])
            {
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going,
                    consumer(StreamItem{concat_lr(null_probe, build_batch.rows[j]), {}}, out_layout));
                if (!keep_going)
                    break;
            }
        }
    }
    return out_layout;
}

popo::base::Result<Batch>
Executor::exec_join(const popo::planner::JoinNode & node, const EvalEnv * outer)
{
    const bool spillable_join = node.join_type == popo::parser::JoinType::Inner
        || node.join_type == popo::parser::JoinType::Left;
    auto collect = [&](const popo::planner::PlanNodePtr & child) -> popo::base::Result<JoinInput>
    {
        JoinInput input;
        std::unique_ptr<SpillWriter> writer;
        auto streamed = stream_rows(
            child, outer,
            [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
            {
                input.batch.schemas = layout.schemas;
                const size_t bytes = estimate_row_bytes(item.row) + 32;
                if (!spillable_join)
                {
                    input.batch.rows.push_back(std::move(item.row));
                    return true;
                }
                if (!writer && memory_ && memory_->try_reserve(bytes))
                {
                    input.charged += bytes;
                    input.batch.rows.push_back(std::move(item.row));
                    return true;
                }
                if (!writer)
                {
                    if (!temp_)
                        return executor_error(
                            "join spill context is not initialized",
                            popo::base::Errno::InternalErr);
                    auto path = temp_->create("join-input");
                    if (!path)
                        return std::unexpected(path.error());
                    input.spill_path = *path;
                    writer = std::make_unique<SpillWriter>(*path, temp_.get());
                    for (const auto & row : input.batch.rows)
                        POPO_TRY(writer->write(row));
                    input.batch.rows.clear();
                    if (memory_)
                        memory_->release(input.charged);
                    input.charged = 0;
                }
                POPO_TRY(writer->write(item.row));
                return true;
            });
        if (!streamed)
            return std::unexpected(streamed.error());
        input.batch.schemas = streamed->schemas;
        if (writer)
            POPO_TRY(writer->close());
        return input;
    };
    POPO_ASSIGN_OR_RETURN(JoinInput left_input, collect(node.left));
    POPO_ASSIGN_OR_RETURN(JoinInput right_input, collect(node.right));
    MemoryChargeGuard charge_guard(memory_.get());
    charge_guard.adopt(left_input.charged + right_input.charged);

    Batch & left = left_input.batch;
    Batch & right = right_input.batch;
    Batch out;
    out.schemas = left.schemas;
    out.schemas.insert(out.schemas.end(), right.schemas.begin(), right.schemas.end());
    if (!temp_)
        return executor_error(
            "join output spill context is not initialized",
            popo::base::Errno::InternalErr);
    auto join_output_path = temp_->create("join-output");
    if (!join_output_path)
        return std::unexpected(join_output_path.error());
    out.spilled_rows = *join_output_path;
    SpillWriter join_output(*join_output_path, temp_.get());
    auto emit_join = [&](Row row) { return join_output.write(row); };

    size_t right_width = right.width();
    Row null_right;
    null_right.reserve_values(right_width);
    for (size_t i = 0; i < right_width; ++i)
        null_right.append_value(Value());
    Row null_left;
    null_left.reserve_values(left.width());
    for (size_t i = 0; i < left.width(); ++i)
        null_left.append_value(Value());

    EvalEnv env;
    env.layout = &out;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;
    auto concat = [](const Row & l, const Row & r) {
        Row row;
        row.reserve_values(l.values_.size() + r.values_.size());
        row.values_ = l.values_;
        row.values_.insert(row.values_.end(), r.values_.begin(), r.values_.end());
        return row;
    };
    auto matches = [&](const Row & l, const Row & r) -> popo::base::Result<bool> {
        if (!node.condition)
            return true;
        Row joined = concat(l, r);
        env.row = &joined;
        POPO_ASSIGN_OR_RETURN(auto condition, eval(node.condition, env));
        return as_bool(condition);
    };

    bool emit_unmatched_left = node.join_type == popo::parser::JoinType::Left || node.join_type == popo::parser::JoinType::Full;
    bool emit_unmatched_right = node.join_type == popo::parser::JoinType::Right || node.join_type == popo::parser::JoinType::Full;
    std::vector<bool> right_matched(right.rows.size(), false);

    std::vector<std::pair<ExprPtr, ExprPtr>> equi_keys;
    if (node.condition)
    {
        auto in_schemas = [](const std::vector<ColumnSchema> & schemas, const std::string & tbl, const std::string & col)
        {
            for (const auto & s : schemas)
            {
                bool tm = tbl.empty() || eq_ci(tbl, s.table) || (!s.alias.empty() && eq_ci(tbl, s.alias));
                if (!tm)
                    continue;
                for (const auto & c : s.columns)
                {
                    if (eq_ci(c, col))
                        return true;
                }
            }
            return false;
        };
        auto side_of = [&](const ExprPtr & side) -> int
        {
            if (!side || expr_has_subquery_or_agg(side))
                return 0;
            std::vector<const popo::parser::ColumnRefExpr *> refs;
            collect_col_refs(side, refs);
            bool has_l = false;
            bool has_r = false;
            for (const auto * ref : refs)
            {
                bool in_l = in_schemas(left.schemas, ref->table_name, ref->column_name);
                bool in_r = in_schemas(right.schemas, ref->table_name, ref->column_name);
                if (in_l && in_r)
                    return 0;
                if (in_l)
                    has_l = true;
                if (in_r)
                    has_r = true;
            }
            if (has_l && !has_r)
                return 1;
            if (has_r && !has_l)
                return 2;
            return 0;
        };
        std::vector<ExprPtr> parts;
        split_and_conjuncts(node.condition, parts);
        for (const auto & p : parts)
        {
            if (!p || p->type != ExprType::BinaryOp)
                continue;
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*p);
            if (b.op != popo::parser::BinaryOpType::Eq)
                continue;
            int sl = side_of(b.left);
            int sr = side_of(b.right);
            if (sl == 1 && sr == 2)
                equi_keys.emplace_back(b.left, b.right);
            else if (sl == 2 && sr == 1)
                equi_keys.emplace_back(b.right, b.left);
        }
    }

    if (!equi_keys.empty())
    {
        EvalEnv left_env{&left, nullptr, outer, nullptr, env.depth};
        EvalEnv right_env{&right, nullptr, outer, nullptr, env.depth};
        size_t build_bytes = 0;
        for (const auto & row : right.rows)
            build_bytes += estimate_row_bytes(row) + 48;
        const bool can_grace_spill = memory_ && temp_
            && (node.join_type == popo::parser::JoinType::Inner
                || node.join_type == popo::parser::JoinType::Left)
            && (left_input.spill_path || right_input.spill_path
                || build_bytes > std::max<size_t>(memory_->limit() / 2, 4096));
        if (can_grace_spill)
        {
            auto decorate = [&](Row row, EvalEnv & key_env, bool left_side)
                -> popo::base::Result<Row>
            {
                key_env.row = &row;
                std::string key;
                bool any_null = false;
                for (const auto & [lexpr, rexpr] : equi_keys)
                {
                    POPO_ASSIGN_OR_RETURN(
                        Value value, eval(left_side ? lexpr : rexpr, key_env));
                    if (is_null(value))
                    {
                        any_null = true;
                        break;
                    }
                    key += value_key(value);
                    key.push_back('\x1f');
                }
                row.append_value(Value(ValueType::String, any_null ? std::string() : key));
                return row;
            };
            constexpr size_t partitions = 32;
            size_t left_pos = 0;
            size_t right_pos = 0;
            std::unique_ptr<SpillReader> left_source;
            std::unique_ptr<SpillReader> right_source;
            if (left_input.spill_path)
                left_source = std::make_unique<SpillReader>(*left_input.spill_path);
            if (right_input.spill_path)
                right_source = std::make_unique<SpillReader>(*right_input.spill_path);
            auto next_raw = [](Batch & batch, size_t & position, std::unique_ptr<SpillReader> & reader)
                -> popo::base::Result<std::optional<Row>>
            {
                if (reader)
                    return reader->next();
                if (position == batch.rows.size())
                    return std::optional<Row>{};
                return std::optional<Row>(std::move(batch.rows[position++]));
            };
            auto hash_decorated = [](const Row & row)
            {
                const auto & key = row.values_.back().string_ref();
                return std::hash<std::string>{}(key);
            };
            auto left_paths = partition_rows(
                [&]() -> popo::base::Result<std::optional<Row>>
                {
                    auto row = next_raw(left, left_pos, left_source);
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        return std::optional<Row>{};
                    POPO_ASSIGN_OR_RETURN(
                        Row decorated, decorate(std::move(**row), left_env, true));
                    return std::optional<Row>(std::move(decorated));
                },
                hash_decorated, partitions, *temp_, "join-left");
            if (!left_paths)
                return std::unexpected(left_paths.error());
            auto right_paths = partition_rows(
                [&]() -> popo::base::Result<std::optional<Row>>
                {
                    auto row = next_raw(right, right_pos, right_source);
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        return std::optional<Row>{};
                    POPO_ASSIGN_OR_RETURN(
                        Row decorated, decorate(std::move(**row), right_env, false));
                    return std::optional<Row>(std::move(decorated));
                },
                hash_decorated, partitions, *temp_, "join-right");
            if (!right_paths)
                return std::unexpected(right_paths.error());
            left.rows.clear();
            right.rows.clear();
            charge_guard.release();

            for (size_t partition = 0; partition < partitions; ++partition)
            {
                std::vector<Row> build;
                std::unordered_map<std::string, std::vector<size_t>> hash_table;
                size_t charged = 0;
                bool skewed = false;
                SpillReader right_reader((*right_paths)[partition]);
                for (;;)
                {
                    auto item = right_reader.next();
                    if (!item)
                        return std::unexpected(item.error());
                    if (!*item)
                        break;
                    const size_t bytes = estimate_row_bytes(**item) + 48;
                    if (!memory_->try_reserve(bytes))
                    {
                        skewed = true;
                        break;
                    }
                    charged += bytes;
                    build.push_back(std::move(**item));
                }
                if (!skewed)
                {
                    for (size_t i = 0; i < build.size(); ++i)
                    {
                        const std::string & key = build[i].values_.back().string_ref();
                        if (!key.empty())
                            hash_table[key].push_back(i);
                    }
                }
                SpillReader left_reader((*left_paths)[partition]);
                for (;;)
                {
                    auto left_item = left_reader.next();
                    if (!left_item)
                    {
                        memory_->release(charged);
                        return std::unexpected(left_item.error());
                    }
                    if (!*left_item)
                        break;
                    Row left_row = std::move(**left_item);
                    const std::string left_key = left_row.values_.back().string_ref();
                    left_row.values_.pop_back();
                    bool matched = false;
                    if (!left_key.empty() && !skewed)
                    {
                        auto found = hash_table.find(left_key);
                        if (found != hash_table.end())
                        {
                            for (size_t index : found->second)
                            {
                                Row & candidate = build[index];
                                Value saved = std::move(candidate.values_.back());
                                candidate.values_.pop_back();
                                POPO_ASSIGN_OR_RETURN(bool pair, matches(left_row, candidate));
                                if (pair)
                                {
                                    matched = true;
                                    POPO_TRY(emit_join(concat(left_row, candidate)));
                                }
                                candidate.append_value(std::move(saved));
                            }
                        }
                    }
                    else if (!left_key.empty())
                    {
                        SpillReader fallback((*right_paths)[partition]);
                        for (;;)
                        {
                            auto right_item = fallback.next();
                            if (!right_item)
                            {
                                memory_->release(charged);
                                return std::unexpected(right_item.error());
                            }
                            if (!*right_item)
                                break;
                            Row right_row = std::move(**right_item);
                            const std::string right_key = right_row.values_.back().string_ref();
                            right_row.values_.pop_back();
                            if (right_key != left_key)
                                continue;
                            POPO_ASSIGN_OR_RETURN(bool pair, matches(left_row, right_row));
                            if (pair)
                            {
                                matched = true;
                                POPO_TRY(emit_join(concat(left_row, right_row)));
                            }
                        }
                    }
                    if (!matched && emit_unmatched_left)
                        POPO_TRY(emit_join(concat(left_row, null_right)));
                }
                memory_->release(charged);
            }
            POPO_TRY(join_output.close());
            return out;
        }
        std::unordered_map<std::string, std::vector<size_t>> hash_table;
        hash_table.reserve(right.rows.size());
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            right_env.row = &right.rows[j];
            std::string key;
            bool any_null = false;
            for (const auto & [_, rexpr] : equi_keys)
            {
                POPO_ASSIGN_OR_RETURN(Value rv, eval(rexpr, right_env));
                if (is_null(rv))
                {
                    any_null = true;
                    break;
                }
                key += value_key(rv);
                key += '\x1f';
            }
            if (!any_null)
                hash_table[key].push_back(j);
        }
        for (const auto & lrow : left.rows)
        {
            left_env.row = &lrow;
            std::string key;
            bool any_null = false;
            for (const auto & [lexpr, _] : equi_keys)
            {
                POPO_ASSIGN_OR_RETURN(Value lv, eval(lexpr, left_env));
                if (is_null(lv))
                {
                    any_null = true;
                    break;
                }
                key += value_key(lv);
                key += '\x1f';
            }
            bool matched = false;
            if (!any_null)
            {
                auto it = hash_table.find(key);
                if (it != hash_table.end())
                {
                    for (size_t j : it->second)
                    {
                        POPO_ASSIGN_OR_RETURN(bool matched_pair, matches(lrow, right.rows[j]));
                        if (matched_pair)
                        {
                            matched = true;
                            right_matched[j] = true;
                            POPO_TRY(emit_join(concat(lrow, right.rows[j])));
                        }
                    }
                }
            }
            if (!matched && emit_unmatched_left)
                POPO_TRY(emit_join(concat(lrow, null_right)));
        }
        if (emit_unmatched_right)
        {
            for (size_t j = 0; j < right.rows.size(); ++j)
            {
                if (!right_matched[j])
                    POPO_TRY(emit_join(concat(null_left, right.rows[j])));
            }
        }
        POPO_TRY(join_output.close());
        return out;
    }

    if (left_input.spill_path || right_input.spill_path)
    {
        size_t left_position = 0;
        std::unique_ptr<SpillReader> left_reader;
        if (left_input.spill_path)
            left_reader = std::make_unique<SpillReader>(*left_input.spill_path);
        for (;;)
        {
            std::optional<Row> left_item;
            if (left_reader)
            {
                auto item = left_reader->next();
                if (!item)
                    return std::unexpected(item.error());
                left_item = std::move(*item);
            }
            else if (left_position < left.rows.size())
                left_item = left.rows[left_position++];
            if (!left_item)
                break;
            bool matched = false;
            if (right_input.spill_path)
            {
                SpillReader right_reader(*right_input.spill_path);
                for (;;)
                {
                    auto right_item = right_reader.next();
                    if (!right_item)
                        return std::unexpected(right_item.error());
                    if (!*right_item)
                        break;
                    POPO_ASSIGN_OR_RETURN(bool pair, matches(*left_item, **right_item));
                    if (pair)
                    {
                        matched = true;
                        POPO_TRY(emit_join(concat(*left_item, **right_item)));
                    }
                }
            }
            else
            {
                for (const auto & right_row : right.rows)
                {
                    POPO_ASSIGN_OR_RETURN(bool pair, matches(*left_item, right_row));
                    if (pair)
                    {
                        matched = true;
                        POPO_TRY(emit_join(concat(*left_item, right_row)));
                    }
                }
            }
            if (!matched && emit_unmatched_left)
                POPO_TRY(emit_join(concat(*left_item, null_right)));
        }
        POPO_TRY(join_output.close());
        return out;
    }

    for (const auto & lrow : left.rows)
    {
        bool matched = false;
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            POPO_ASSIGN_OR_RETURN(bool matched_pair, matches(lrow, right.rows[j]));
            if (matched_pair)
            {
                matched = true;
                right_matched[j] = true;
                POPO_TRY(emit_join(concat(lrow, right.rows[j])));
            }
        }
        if (!matched && emit_unmatched_left)
            POPO_TRY(emit_join(concat(lrow, null_right)));
    }
    if (emit_unmatched_right)
    {
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            if (!right_matched[j])
                POPO_TRY(emit_join(concat(null_left, right.rows[j])));
        }
    }
    POPO_TRY(join_output.close());
    return out;
}

} // namespace popo::executor
