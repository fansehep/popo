#include "executor/executor.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/external_sorter.h"
#include "executor/types/top_item.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::is_null;
using popo::function::sql_eq;
using popo::function::sql_lt;
using popo::parser::Expr;
using popo::type::Row;
using popo::type::Value;

popo::base::Result<Batch>
Executor::exec_topn_sort(
    const popo::planner::SortNode & node, int64_t limit_count, int64_t offset_count,
    const EvalEnv * outer)
{
    const int64_t offset = std::max<int64_t>(offset_count, 0);
    if (limit_count < 0 || limit_count > 1000000 || offset > 1000000)
    {
        POPO_ASSIGN_OR_RETURN(Batch sorted, exec_sort(node, outer));
        Batch out;
        out.schemas = sorted.schemas;
        const bool has_aggs = !sorted.row_aggs.empty();
        int64_t rem = limit_count < 0 ? static_cast<int64_t>(sorted.rows.size()) : limit_count;
        for (size_t i = static_cast<size_t>(offset); i < sorted.rows.size() && rem > 0; ++i, --rem)
        {
            out.rows.push_back(std::move(sorted.rows[i]));
            if (has_aggs)
                out.row_aggs.push_back(std::move(sorted.row_aggs[i]));
        }
        return out;
    }

    const size_t keep = static_cast<size_t>(offset + limit_count);
    const size_t key_count = node.order_by.size();
    Batch input_layout;
    std::vector<const Expr *> aggregate_exprs;

    auto less = [&](const TopItem & a, const TopItem & b) -> bool
    {
        for (size_t k = 0; k < key_count; ++k)
        {
            const auto & av = a.keys[k];
            const auto & bv = b.keys[k];
            const bool asc = node.order_by[k].ascending;
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
        return a.seq < b.seq;
    };

    if (keep == 0)
    {
        auto streamed = stream_rows(
            node.child, outer,
            [&](StreamItem &&, const Batch & layout) -> popo::base::Result<bool>
            {
                input_layout.schemas = layout.schemas;
                return false;
            });
        if (!streamed)
            return std::unexpected(streamed.error());
        if (input_layout.schemas.empty())
            input_layout.schemas = streamed->schemas;
        return input_layout;
    }

    std::vector<TopItem> heap;
    heap.reserve(std::min<size_t>(keep + 1, 1024));
    uint64_t seq = 0;
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            if (input_layout.schemas.empty())
                input_layout.schemas = layout.schemas;
            if (aggregate_exprs.empty() && !item.aggs.empty())
                for (const auto & [expr, _] : item.aggs)
                    aggregate_exprs.push_back(expr);
            EvalEnv env{&layout, &item.row, outer,
                        item.aggs.empty() ? nullptr : &item.aggs,
                        outer ? outer->depth + 1 : 0};
            TopItem cand;
            cand.keys.reserve(key_count);
            for (const auto & order : node.order_by)
            {
                POPO_ASSIGN_OR_RETURN(Value key, eval(order.expr, env));
                cand.keys.push_back(std::move(key));
            }
            cand.seq = seq++;
            if (heap.size() < keep)
            {
                cand.row = std::move(item.row);
                cand.aggs = std::move(item.aggs);
                heap.push_back(std::move(cand));
                std::push_heap(heap.begin(), heap.end(), less);
            }
            else if (less(cand, heap.front()))
            {
                cand.row = std::move(item.row);
                cand.aggs = std::move(item.aggs);
                std::pop_heap(heap.begin(), heap.end(), less);
                heap.back() = std::move(cand);
                std::push_heap(heap.begin(), heap.end(), less);
            }
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    if (input_layout.schemas.empty())
        input_layout.schemas = streamed->schemas;

    std::sort_heap(heap.begin(), heap.end(), less);

    Batch out;
    out.schemas = input_layout.schemas;
    const bool has_aggs = !aggregate_exprs.empty();
    for (size_t i = static_cast<size_t>(offset); i < heap.size(); ++i)
    {
        out.rows.push_back(std::move(heap[i].row));
        if (has_aggs)
            out.row_aggs.push_back(std::move(heap[i].aggs));
    }
    return out;
}

popo::base::Result<Batch>
Executor::exec_sort(const popo::planner::SortNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("sort spill context is not initialized", popo::base::Errno::InternalErr);

    Batch input_layout;
    std::vector<const Expr *> aggregate_exprs;
    const size_t key_count = node.order_by.size();
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t k = 0; k < key_count; ++k)
        {
            const auto & av = a.values_[k];
            const auto & bv = b.values_[k];
            const bool asc = node.order_by[k].ascending;
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
    };
    ExternalSorter sorter(less, *memory_, *temp_);
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            input_layout.schemas = layout.schemas;
            if (aggregate_exprs.empty() && !item.aggs.empty())
                for (const auto & [expr, _] : item.aggs)
                    aggregate_exprs.push_back(expr);
            EvalEnv env{&layout, &item.row, outer,
                        item.aggs.empty() ? nullptr : &item.aggs,
                        outer ? outer->depth + 1 : 0};
            Row decorated;
            decorated.reserve_values(key_count + aggregate_exprs.size() + item.row.values_.size());
            for (const auto & order : node.order_by)
            {
                POPO_ASSIGN_OR_RETURN(Value key, eval(order.expr, env));
                decorated.append_value(std::move(key));
            }
            for (const Expr * expr : aggregate_exprs)
            {
                auto it = item.aggs.find(expr);
                decorated.append_value(it == item.aggs.end() ? Value() : it->second);
            }
            for (auto & value : item.row.values_)
                decorated.append_value(std::move(value));
            POPO_TRY(sorter.push(std::move(decorated)));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    input_layout.schemas = streamed->schemas;

    auto sorted = sorter.finish();
    if (!sorted)
        return std::unexpected(sorted.error());

    Batch out;
    out.schemas = input_layout.schemas;
    out.rows.reserve(sorted->size());
    const size_t prefix = key_count + aggregate_exprs.size();
    for (auto & decorated : *sorted)
    {
        Row row;
        row.reserve_values(decorated.values_.size() - prefix);
        for (size_t i = prefix; i < decorated.values_.size(); ++i)
            row.append_value(std::move(decorated.values_[i]));
        out.rows.push_back(std::move(row));
        if (!aggregate_exprs.empty())
        {
            std::unordered_map<const Expr *, Value> aggs;
            for (size_t i = 0; i < aggregate_exprs.size(); ++i)
                aggs.emplace(aggregate_exprs[i], std::move(decorated.values_[key_count + i]));
            out.row_aggs.push_back(std::move(aggs));
        }
    }
    return out;
}

popo::base::Result<Batch>
Executor::exec_limit(const popo::planner::LimitNode & node, const EvalEnv * outer)
{
    if (node.limit_count >= 0 && node.child && node.child->type == popo::planner::PlanNodeType::Sort)
    {
        return exec_topn_sort(
            static_cast<const popo::planner::SortNode &>(*node.child),
            node.limit_count, node.offset_count, outer);
    }
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    out.schemas = batch.schemas;
    const bool has_aggs = !batch.row_aggs.empty();
    const int64_t offset = std::max<int64_t>(node.offset_count, 0);
    int64_t remaining = node.limit_count < 0 ? static_cast<int64_t>(batch.rows.size()) : node.limit_count;
    for (size_t i = static_cast<size_t>(offset); i < batch.rows.size() && remaining > 0; ++i, --remaining)
    {
        out.rows.push_back(std::move(batch.rows[i]));
        if (has_aggs)
            out.row_aggs.push_back(std::move(batch.row_aggs[i]));
    }
    return out;
}

} // namespace popo::executor
