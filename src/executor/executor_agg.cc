#include "executor/executor.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/external_sorter.h"
#include "executor/spill/spill_codec.h"
#include "executor/types/agg_state.h"
#include "executor/types/hash_agg_state.h"
#include "executor/vector/flat_int64_map.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::do_arithmetic;
using popo::function::is_floating;
using popo::function::is_integer_value;
using popo::function::is_null;
using popo::function::row_key;
using popo::function::sql_eq;
using popo::function::sql_lt;
using popo::function::to_i64;
using popo::function::value_key;
using popo::parser::Expr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<Batch>
Executor::exec_agg(const popo::planner::AggNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("aggregation spill context is not initialized", popo::base::Errno::InternalErr);

    {
        POPO_ASSIGN_OR_RETURN(auto vec_batch, try_exec_vectorized_agg(node, outer));
        if (vec_batch)
            return std::move(*vec_batch);
    }

    const size_t group_count = node.group_by.size();
    const size_t agg_count = node.agg_exprs.size();

    {
        Batch out;
        std::vector<Row> reps;
        std::vector<std::vector<HashAggState>> group_states;
        std::unordered_map<std::string, uint32_t> str_groups;
        FlatInt64Map<uint32_t> i64_groups;
        bool use_i64_key = (group_count == 1);
        size_t charged = 0;
        bool overflowed = false;
        size_t row_width = 0;

        auto release_all = [&]()
        {
            if (charged > 0)
            {
                memory_->release(charged);
                charged = 0;
            }
        };

        if (group_count == 0)
            group_states.emplace_back(agg_count);

        std::vector<Value> gvals(group_count);
        std::vector<Value> avals(agg_count);
        std::string gkey_buf;

        auto streamed = stream_rows(
            node.child, outer,
            [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
            {
                if (out.schemas.empty())
                {
                    out.schemas = layout.schemas;
                    row_width = item.row.values_.size();
                }
                EvalEnv env{&layout, &item.row, outer,
                            item.aggs.empty() ? nullptr : &item.aggs,
                            outer ? outer->depth + 1 : 0};

                for (size_t i = 0; i < group_count; ++i)
                {
                    POPO_ASSIGN_OR_RETURN(gvals[i], eval(node.group_by[i], env));
                }
                for (size_t i = 0; i < agg_count; ++i)
                {
                    const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
                    const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
                    if (star)
                        avals[i] = Value(ValueType::Int32, 1);
                    else
                    {
                        POPO_ASSIGN_OR_RETURN(avals[i], eval(func.args[0], env));
                    }
                }

                uint32_t gid = 0;
                if (group_count == 0)
                {
                    if (reps.empty())
                        reps.push_back(std::move(item.row));
                }
                else
                {
                    if (use_i64_key && (is_null(gvals[0]) || !is_integer_value(gvals[0])))
                    {
                        use_i64_key = false;
                        i64_groups.for_each([&](int64_t k, uint32_t id) {
                            std::string sk = value_key(Value(ValueType::Int64, k));
                            sk.push_back('\x1f');
                            str_groups.emplace(std::move(sk), id);
                        });
                        i64_groups = FlatInt64Map<uint32_t>{};
                    }

                    bool is_new = false;
                    if (use_i64_key)
                    {
                        const int64_t k = to_i64(gvals[0]);
                        const uint32_t * found = i64_groups.find(k);
                        if (found)
                        {
                            gid = *found;
                        }
                        else
                        {
                            const size_t need = estimate_row_bytes(item.row) + agg_count * sizeof(HashAggState) + 64;
                            if (!memory_->try_reserve(need))
                            {
                                overflowed = true;
                                return false;
                            }
                            charged += need;
                            gid = static_cast<uint32_t>(reps.size());
                            bool ins = false;
                            i64_groups.get_or_insert(k, ins) = gid;
                            is_new = true;
                        }
                    }
                    else
                    {
                        gkey_buf.clear();
                        for (size_t i = 0; i < group_count; ++i)
                        {
                            gkey_buf += value_key(gvals[i]);
                            gkey_buf.push_back('\x1f');
                        }
                        auto it = str_groups.find(gkey_buf);
                        if (it != str_groups.end())
                        {
                            gid = it->second;
                        }
                        else
                        {
                            const size_t need = estimate_row_bytes(item.row) + gkey_buf.size()
                                + agg_count * sizeof(HashAggState) + 80;
                            if (!memory_->try_reserve(need))
                            {
                                overflowed = true;
                                return false;
                            }
                            charged += need;
                            gid = static_cast<uint32_t>(reps.size());
                            str_groups.emplace(gkey_buf, gid);
                            is_new = true;
                        }
                    }

                    if (is_new)
                    {
                        reps.push_back(std::move(item.row));
                        group_states.emplace_back(agg_count);
                    }
                }

                auto & states = group_states[gid];
                for (size_t i = 0; i < agg_count; ++i)
                {
                    const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
                    const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
                    const Value & val = avals[i];
                    if (!star && is_null(val))
                        continue;
                    auto & st = states[i];
                    if (func.is_distinct)
                    {
                        std::string dk = value_key(val);
                        if (st.distinct_seen.contains(dk))
                            continue;
                        const size_t dbytes = dk.size() + 48;
                        if (!memory_->try_reserve(dbytes))
                        {
                            overflowed = true;
                            return false;
                        }
                        charged += dbytes;
                        st.distinct_seen.insert(std::move(dk));
                    }
                    switch (func.func)
                    {
                        case popo::parser::AggFuncType::Count:
                            ++st.count;
                            break;
                        case popo::parser::AggFuncType::Sum:
                        case popo::parser::AggFuncType::Avg:
                            if (!st.initialized)
                            {
                                if (is_floating(val))
                                {
                                    st.is_float_sum = true;
                                    st.sum_f64 = val.to_f64();
                                }
                                else if (is_integer_value(val))
                                {
                                    st.is_float_sum = false;
                                    st.sum_i64 = to_i64(val);
                                }
                                else
                                {
                                    st.is_float_sum = true;
                                    st.sum_f64 = val.to_f64();
                                }
                                st.initialized = true;
                            }
                            else
                            {
                                if (st.is_float_sum)
                                {
                                    st.sum_f64 += val.to_f64();
                                }
                                else if (is_floating(val))
                                {
                                    st.is_float_sum = true;
                                    st.sum_f64 = static_cast<double>(st.sum_i64) + val.to_f64();
                                }
                                else
                                {
                                    st.sum_i64 += to_i64(val);
                                }
                            }
                            ++st.count;
                            break;
                        case popo::parser::AggFuncType::Min:
                            if (!st.initialized || sql_lt(val, st.val_result))
                            {
                                st.val_result = val;
                                st.initialized = true;
                            }
                            break;
                        case popo::parser::AggFuncType::Max:
                            if (!st.initialized || sql_lt(st.val_result, val))
                            {
                                st.val_result = val;
                                st.initialized = true;
                            }
                            break;
                        case popo::parser::AggFuncType::XmlAgg: {
                            const std::string piece = val.datatype() == ValueType::String
                                ? std::string(val.string_ref()) : val.to_val_string();
                            st.val_result = Value(
                                ValueType::String,
                                st.initialized ? st.val_result.string_ref() + piece : piece);
                            st.initialized = true;
                            break;
                        }
                    }
                }
                return true;
            });

        release_all();
        if (!overflowed)
        {
            if (!streamed)
                return std::unexpected(streamed.error());
            if (out.schemas.empty())
            {
                out.schemas = streamed->schemas;
                row_width = streamed->width();
            }
            if (group_count == 0 && reps.empty())
            {
                Row empty_rep;
                empty_rep.reserve_values(row_width);
                for (size_t i = 0; i < row_width; ++i)
                    empty_rep.append_value(Value());
                reps.push_back(std::move(empty_rep));
            }
            out.rows.reserve(reps.size());
            out.row_aggs.reserve(reps.size());
            for (size_t g = 0; g < reps.size(); ++g)
            {
                const auto & states = group_states[g];
                std::unordered_map<const Expr *, Value> agg_values;
                agg_values.reserve(agg_count);
                for (size_t i = 0; i < agg_count; ++i)
                {
                    const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
                    const auto & st = states[i];
                    Value value;
                    switch (func.func)
                    {
                        case popo::parser::AggFuncType::Count:
                            value = Value(ValueType::Int64, st.count);
                            break;
                        case popo::parser::AggFuncType::Sum:
                            if (st.initialized)
                                value = st.is_float_sum
                                    ? Value(ValueType::Float64, st.sum_f64)
                                    : Value(ValueType::Int64, st.sum_i64);
                            break;
                        case popo::parser::AggFuncType::Avg:
                            if (st.count > 0)
                            {
                                double s = st.is_float_sum ? st.sum_f64 : static_cast<double>(st.sum_i64);
                                value = Value(ValueType::Float64, s / static_cast<double>(st.count));
                            }
                            break;
                        default:
                            value = st.val_result;
                            break;
                    }
                    agg_values.emplace(node.agg_exprs[i].get(), std::move(value));
                }
                out.rows.push_back(std::move(reps[g]));
                out.row_aggs.push_back(std::move(agg_values));
            }
            return out;
        }
    }

    Batch out;
    size_t row_width = 0;
    size_t primary_distinct = agg_count;
    for (size_t i = 0; i < agg_count; ++i)
    {
        const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
        if (func.is_distinct)
        {
            primary_distinct = i;
            break;
        }
    }
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t i = 0; i < group_count; ++i)
        {
            if (sql_eq(a.values_[i], b.values_[i]) || (is_null(a.values_[i]) && is_null(b.values_[i])))
                continue;
            if (is_null(a.values_[i]))
                return true;
            if (is_null(b.values_[i]))
                return false;
            return sql_lt(a.values_[i], b.values_[i]);
        }
        if (primary_distinct < agg_count)
        {
            const auto & av = a.values_[group_count + primary_distinct];
            const auto & bv = b.values_[group_count + primary_distinct];
            if (!sql_eq(av, bv) && !(is_null(av) && is_null(bv)))
            {
                if (is_null(av))
                    return true;
                if (is_null(bv))
                    return false;
                return sql_lt(av, bv);
            }
        }
        return false;
    };
    ExternalSorter sorter(less, *memory_, *temp_);
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            out.schemas = layout.schemas;
            row_width = item.row.values_.size();
            EvalEnv env{&layout, &item.row, outer,
                        item.aggs.empty() ? nullptr : &item.aggs,
                        outer ? outer->depth + 1 : 0};
            Row decorated;
            decorated.reserve_values(group_count + agg_count + row_width);
            for (const auto & group : node.group_by)
            {
                POPO_ASSIGN_OR_RETURN(Value value, eval(group, env));
                decorated.append_value(std::move(value));
            }
            for (const auto & expression : node.agg_exprs)
            {
                const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*expression);
                const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
                if (star)
                    decorated.append_value(Value(ValueType::Int32, 1));
                else
                {
                    POPO_ASSIGN_OR_RETURN(Value value, eval(func.args[0], env));
                    decorated.append_value(std::move(value));
                }
            }
            for (auto & value : item.row.values_)
                decorated.append_value(std::move(value));
            POPO_TRY(sorter.push(std::move(decorated)));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    out.schemas = streamed->schemas;
    row_width = streamed->width();

    std::vector<AggState> states(agg_count);
    Row representative;
    std::string current_group;
    bool have_group = false;
    auto emit_group = [&]() -> popo::base::Result<void>
    {
        if (!have_group)
            return {};
        std::unordered_map<const Expr *, Value> agg_values;
        for (size_t i = 0; i < agg_count; ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            const auto & state = states[i];
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
        out.rows.push_back(representative);
        out.row_aggs.push_back(std::move(agg_values));
        return {};
    };
    POPO_TRY(sorter.for_each([&](Row && decorated) -> popo::base::Result<bool>
    {
        std::string group_key;
        for (size_t i = 0; i < group_count; ++i)
        {
            group_key += value_key(decorated.values_[i]);
            group_key.push_back('\x1f');
        }
        if (!have_group || group_key != current_group)
        {
            POPO_TRY(emit_group());
            current_group = std::move(group_key);
            have_group = true;
            states.assign(agg_count, AggState{});
            representative = Row{};
            representative.reserve_values(row_width);
            for (size_t i = 0; i < row_width; ++i)
                representative.append_value(decorated.values_[group_count + agg_count + i]);
        }
        for (size_t i = 0; i < agg_count; ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            auto & state = states[i];
            const Value & val = decorated.values_[group_count + i];
            const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
            if (!star && is_null(val))
                continue;
            if (func.is_distinct)
            {
                const std::string key = value_key(val);
                if (i == primary_distinct)
                {
                    if (state.has_last_distinct && state.last_distinct == key)
                        continue;
                    state.last_distinct = key;
                    state.has_last_distinct = true;
                }
                else if (!state.distinct_seen.insert(key).second)
                    continue;
            }
            switch (func.func)
            {
                case popo::parser::AggFuncType::Count: ++state.count; break;
                case popo::parser::AggFuncType::Sum:
                case popo::parser::AggFuncType::Avg:
                    if (state.initialized)
                    {
                        POPO_ASSIGN_OR_RETURN(state.result, do_arithmetic('+', state.result, val));
                    }
                    else
                        state.result = val;
                    state.initialized = true;
                    ++state.count;
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
                    const std::string piece = val.datatype() == ValueType::String
                        ? std::string(val.string_ref()) : val.to_val_string();
                    state.result = Value(
                        ValueType::String,
                        state.initialized ? state.result.string_ref() + piece : piece);
                    state.initialized = true;
                    break;
                }
            }
        }
        return true;
    }));
    POPO_TRY(emit_group());
    if (!have_group && group_count == 0)
    {
        have_group = true;
        representative = Row{};
        representative.reserve_values(row_width);
        for (size_t i = 0; i < row_width; ++i)
            representative.append_value(Value());
        POPO_TRY(emit_group());
    }
    return out;
}

popo::base::Result<Batch>
Executor::exec_distinct(const popo::planner::DistinctNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("distinct spill context is not initialized", popo::base::Errno::InternalErr);
    Batch out;
    std::vector<const Expr *> aggregate_exprs;
    size_t row_width = 0;
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t i = 0; i < row_width; ++i)
        {
            if (sql_eq(a.values_[i], b.values_[i]) || (is_null(a.values_[i]) && is_null(b.values_[i])))
                continue;
            if (is_null(a.values_[i]))
                return true;
            if (is_null(b.values_[i]))
                return false;
            return sql_lt(a.values_[i], b.values_[i]);
        }
        return false;
    };
    ExternalSorter sorter(less, *memory_, *temp_);
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            out.schemas = layout.schemas;
            row_width = item.row.values_.size();
            if (aggregate_exprs.empty() && !item.aggs.empty())
                for (const auto & [expr, _] : item.aggs)
                    aggregate_exprs.push_back(expr);
            for (const Expr * expr : aggregate_exprs)
            {
                auto it = item.aggs.find(expr);
                item.row.append_value(it == item.aggs.end() ? Value() : it->second);
            }
            POPO_TRY(sorter.push(std::move(item.row)));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    out.schemas = streamed->schemas;
    std::string previous;
    bool have_previous = false;
    POPO_TRY(sorter.for_each([&](Row && decorated) -> popo::base::Result<bool>
    {
        Row row;
        row.reserve_values(row_width);
        for (size_t i = 0; i < row_width; ++i)
            row.append_value(std::move(decorated.values_[i]));
        std::string key = row_key(row);
        if (have_previous && key == previous)
            return true;
        previous = std::move(key);
        have_previous = true;
        out.rows.push_back(std::move(row));
        if (!aggregate_exprs.empty())
        {
            std::unordered_map<const Expr *, Value> aggs;
            for (size_t i = 0; i < aggregate_exprs.size(); ++i)
                aggs.emplace(aggregate_exprs[i], std::move(decorated.values_[row_width + i]));
            out.row_aggs.push_back(std::move(aggs));
        }
        return true;
    }));
    return out;
}

popo::base::Result<Batch>
Executor::exec_set_op(const popo::planner::SetOpNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto left, exec_batch(node.left, outer));
    POPO_ASSIGN_OR_RETURN(auto right, exec_batch(node.right, outer));
    if (left.width() != right.width())
        return executor_error("set operation branches must have the same number of columns");
    Batch out;
    out.schemas = left.schemas;

    auto counts = [](const std::vector<Row> & rows)
    {
        std::unordered_map<std::string, int> n;
        for (const auto & row : rows)
            n[row_key(row)]++;
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
                    deduped.push_back(std::move(row));
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
                right_count[key] = have - 1;
            else
                keep = true;
        }
        else
        {
            keep = have == 0 && used == 0;
        }
        emitted[key] = used + 1;
        if (keep)
            out.rows.push_back(std::move(row));
    }
    return out;
}

popo::base::Result<Batch>
Executor::exec_recursive_scan(const popo::planner::RecursiveScanNode & node)
{
    auto it = recursive_inputs_.find(node.cte_name);
    if (it == recursive_inputs_.end())
        return {};
    return it->second;
}

popo::base::Result<Batch>
Executor::exec_recursive(const popo::planner::SubqueryScanNode & node)
{
    const auto * setop = dynamic_cast<const popo::planner::SetOpNode *>(node.child.get());
    if (!setop || setop->kind != popo::parser::SetOpKind::Union || !setop->is_all)
        return executor_error("WITH RECURSIVE requires a UNION ALL body");
    POPO_ASSIGN_OR_RETURN(auto anchor, exec_batch(setop->left, nullptr));
    Batch working = anchor;
    Batch delta = anchor;
    auto anchor_names = anchor.flat_names();
    for (int step = 0; step < 1000; ++step)
    {
        recursive_inputs_[node.cte_name] = delta;
        POPO_ASSIGN_OR_RETURN(auto next, exec_batch(setop->right, nullptr));
        if (next.rows.empty())
            break;
        if (step == 999)
        {
            recursive_inputs_.erase(node.cte_name);
            return executor_error("WITH RECURSIVE exceeded 1000 iterations");
        }
        if (next.flat_names().size() == anchor_names.size())
        {
            size_t cursor = 0;
            for (auto & schema : next.schemas)
            {
                for (auto & col : schema.columns)
                    col = anchor_names[cursor++];
            }
        }
        delta.schemas = next.schemas;
        delta.rows = next.rows;
        working.rows.insert(working.rows.end(), next.rows.begin(), next.rows.end());
    }
    recursive_inputs_.erase(node.cte_name);
    return working;
}

popo::base::Result<Batch>
Executor::exec_window(const popo::planner::WindowNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    if (batch.row_aggs.size() < batch.rows.size())
        batch.row_aggs.resize(batch.rows.size());
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;

    for (const auto & wexpr : node.windows)
    {
        const auto & win = static_cast<const popo::parser::WindowExpr &>(*wexpr);
        std::unordered_map<std::string, std::vector<size_t>> parts;
        std::vector<std::vector<Value>> order_keys(batch.rows.size());
        for (size_t i = 0; i < batch.rows.size(); ++i)
        {
            env.row = &batch.rows[i];
            std::string key;
            for (const auto & part : win.partition_by)
            {
                POPO_ASSIGN_OR_RETURN(auto part_value, eval(part, env));
                key += value_key(part_value);
                key += '\x1f';
            }
            for (const auto & item : win.order_by)
            {
                POPO_ASSIGN_OR_RETURN(auto order_value, eval(item.expr, env));
                order_keys[i].push_back(std::move(order_value));
            }
            parts[key].push_back(i);
        }
        auto peer = [&](size_t a, size_t b)
        {
            for (size_t k = 0; k < win.order_by.size(); ++k)
            {
                if (!sql_eq(order_keys[a][k], order_keys[b][k]))
                    return false;
            }
            return true;
        };
        std::string fname;
        const popo::parser::FunctionCallExpr * agg = nullptr;
        if (win.function && win.function->type == ExprType::ScalarCall)
            fname = static_cast<const popo::parser::ScalarCallExpr &>(*win.function).name;
        else if (win.function && win.function->type == ExprType::FunctionCall)
            agg = &static_cast<const popo::parser::FunctionCallExpr &>(*win.function);
        for (auto & [_, idxs] : parts)
        {
            std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b)
            {
                for (size_t k = 0; k < win.order_by.size(); ++k)
                {
                    const auto & item = win.order_by[k];
                    const auto & va = order_keys[a][k];
                    const auto & vb = order_keys[b][k];
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
                    bool new_peer = i == 0 || !peer(idxs[i], idxs[i - 1]);
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
                auto accumulate =
                    [&](size_t index, Value & acc, bool & init, int64_t & count) -> popo::base::Result<void>
                {
                    env.row = &batch.rows[index];
                    bool is_star = agg->args.empty() || agg->args[0]->type == ExprType::Star;
                    Value val(ValueType::Int32, 1);
                    if (!is_star)
                    {
                        POPO_ASSIGN_OR_RETURN(val, eval(agg->args[0], env));
                    }
                    if (!is_star && is_null(val))
                        return {};
                    if (agg->func == popo::parser::AggFuncType::Count)
                    {
                        ++count;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Avg || agg->func == popo::parser::AggFuncType::Sum)
                    {
                        if (init)
                        {
                            POPO_ASSIGN_OR_RETURN(acc, do_arithmetic('+', acc, val));
                        }
                        else
                            acc = val;
                        init = true;
                        ++count;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Min)
                    {
                        if (!init || sql_lt(val, acc))
                            acc = val;
                        init = true;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Max)
                    {
                        if (!init || sql_lt(acc, val))
                            acc = val;
                        init = true;
                    }
                    return {};
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
                        POPO_TRY(accumulate(index, acc, init, count));
                    Value value = finish(acc, init, count);
                    for (size_t index : idxs)
                        batch.row_aggs[index][wexpr.get()] = value;
                }
                else
                {
                    Value acc;
                    bool init = false;
                    int64_t count = 0;
                    for (size_t index : idxs)
                    {
                        POPO_TRY(accumulate(index, acc, init, count));
                        batch.row_aggs[index][wexpr.get()] = finish(acc, init, count);
                    }
                }
            }
        }
    }
    return batch;
}

} // namespace popo::executor
