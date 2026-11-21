#include "executor/executor.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/spill/spill_codec.h"
#include "executor/spill/spill_reader.h"
#include "executor/spill/spill_writer.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::agg_display_name;
using popo::function::as_bool;
using popo::function::eq_ci;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;

popo::base::Result<Batch> Executor::stream_rows(
    const popo::planner::PlanNodePtr & node, const EvalEnv * outer,
    const StreamConsumer & consumer)
{
    if (!node)
        return Batch{};
    using popo::planner::PlanNodeType;
    if (node->type == PlanNodeType::Scan)
    {
        const auto & scan = static_cast<const popo::planner::ScanNode &>(*node);
        return stream_scan_filter_vectorized(
            scan, nullptr, outer,
            [&](ColumnarBatch & batch, const Batch & layout) -> popo::base::Result<bool>
            {
                for (size_t i = 0; i < batch.count; ++i)
                {
                    POPO_ASSIGN_OR_RETURN(
                        bool keep_going,
                        consumer(StreamItem{batch.extract_row(i), {}}, layout));
                    if (!keep_going)
                        return false;
                }
                return true;
            });
    }
    if (node->type == PlanNodeType::Dual)
    {
        Batch layout;
        POPO_ASSIGN_OR_RETURN(bool ignored, consumer(StreamItem{Row{}, {}}, layout));
        (void) ignored;
        return layout;
    }
    if (node->type == PlanNodeType::SubqueryScan)
    {
        const auto & subquery = static_cast<const popo::planner::SubqueryScanNode &>(*node);
        if (subquery.recursive)
        {
            POPO_ASSIGN_OR_RETURN(Batch batch, exec_subquery_scan(subquery, outer));
            Batch layout;
            layout.schemas = batch.schemas;
            for (auto & row : batch.rows)
            {
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going, consumer(StreamItem{std::move(row), {}}, layout));
                if (!keep_going)
                    break;
            }
            return layout;
        }
        auto alias_layout = [&](const Batch & child)
        {
            Batch layout;
            ColumnSchema schema;
            schema.table = subquery.alias;
            schema.alias = subquery.alias;
            schema.columns = subquery.column_names.empty()
                ? child.flat_names() : subquery.column_names;
            layout.schemas.push_back(std::move(schema));
            return layout;
        };
        if (!subquery.cte_name.empty())
        {
            auto cached = stream_materialized_.find(subquery.child.get());
            if (cached != stream_materialized_.end())
            {
                Batch output_layout = alias_layout(cached->second.layout);
                if (!cached->second.path)
                {
                    for (const auto & row : cached->second.in_memory_rows)
                    {
                        POPO_ASSIGN_OR_RETURN(
                            bool keep_going,
                            consumer(StreamItem{row, {}}, output_layout));
                        if (!keep_going)
                            break;
                    }
                    return output_layout;
                }
                SpillReader reader(*cached->second.path);
                for (;;)
                {
                    auto row = reader.next();
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        break;
                    POPO_ASSIGN_OR_RETURN(
                        bool keep_going,
                        consumer(StreamItem{std::move(**row), {}}, output_layout));
                    if (!keep_going)
                        break;
                }
                return output_layout;
            }
            if (!temp_)
                return executor_error(
                    "CTE spill context is not initialized",
                    popo::base::Errno::InternalErr);
            StreamMaterialized mat;
            std::unique_ptr<SpillWriter> writer;
            size_t cte_charged = 0;
            Batch output_layout;
            bool downstream_open = true;
            auto cte_plan = push_projections(optimize_plan(subquery.child, false));
            auto child = stream_rows(
                cte_plan, nullptr,
                [&](StreamItem && item, const Batch & child_layout)
                    -> popo::base::Result<bool>
                {
                    if (output_layout.schemas.empty())
                        output_layout = alias_layout(child_layout);
                    const size_t rbytes = estimate_row_bytes(item.row) + 32;
                    if (!mat.path && (!memory_ || memory_->try_reserve(rbytes)))
                    {
                        if (memory_)
                            cte_charged += rbytes;
                        mat.in_memory_rows.push_back(item.row);
                    }
                    else
                    {
                        if (!mat.path)
                        {
                            auto path = temp_->create("materialized-cte");
                            if (!path)
                                return std::unexpected(path.error());
                            mat.path = *path;
                            writer = std::make_unique<SpillWriter>(*mat.path, temp_.get());
                            for (const auto & buffered : mat.in_memory_rows)
                                POPO_TRY(writer->write(buffered));
                            mat.in_memory_rows.clear();
                            if (memory_ && cte_charged > 0)
                            {
                                memory_->release(cte_charged);
                                cte_charged = 0;
                            }
                        }
                        POPO_TRY(writer->write(item.row));
                    }
                    if (downstream_open)
                    {
                        POPO_ASSIGN_OR_RETURN(
                            downstream_open,
                            consumer(
                                StreamItem{std::move(item.row), std::move(item.aggs)},
                                output_layout));
                    }
                    return true;
                });
            if (!child)
            {
                if (memory_ && cte_charged > 0)
                    memory_->release(cte_charged);
                return std::unexpected(child.error());
            }
            if (output_layout.schemas.empty())
                output_layout = alias_layout(*child);
            if (writer)
                POPO_TRY(writer->close());
            mat.layout = *child;
            stream_materialized_.emplace(subquery.child.get(), std::move(mat));
            return output_layout;
        }

        Batch output_layout;
        auto child = stream_rows(
            subquery.child, outer,
            [&](StreamItem && item, const Batch & child_layout)
                -> popo::base::Result<bool>
            {
                if (output_layout.schemas.empty())
                    output_layout = alias_layout(child_layout);
                return consumer(std::move(item), output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout = alias_layout(*child);
        return output_layout;
    }
    if (node->type == PlanNodeType::Filter)
    {
        const popo::planner::ScanNode * scan = nullptr;
        ExprPtr combined_filter;
        if (extract_scan_and_filters(node, scan, combined_filter))
        {
            return stream_scan_filter_vectorized(
                *scan, combined_filter, outer,
                [&](ColumnarBatch & batch, const Batch & layout) -> popo::base::Result<bool>
                {
                    if (batch.has_sel)
                    {
                        for (size_t s = 0; s < batch.sel_count; ++s)
                        {
                            POPO_ASSIGN_OR_RETURN(
                                bool keep_going,
                                consumer(StreamItem{batch.extract_row(batch.sel[s]), {}}, layout));
                            if (!keep_going)
                                return false;
                        }
                    }
                    else
                    {
                        for (size_t i = 0; i < batch.count; ++i)
                        {
                            POPO_ASSIGN_OR_RETURN(
                                bool keep_going,
                                consumer(StreamItem{batch.extract_row(i), {}}, layout));
                            if (!keep_going)
                                return false;
                        }
                    }
                    return true;
                });
        }
        const auto & filter = static_cast<const popo::planner::FilterNode &>(*node);
        if (filter.child && filter.child->type == PlanNodeType::Agg)
        {
            POPO_ASSIGN_OR_RETURN(
                auto vec_agg,
                try_exec_vectorized_agg(
                    static_cast<const popo::planner::AggNode &>(*filter.child),
                    outer, filter.condition));
            if (vec_agg)
            {
                Batch layout;
                layout.schemas = vec_agg->schemas;
                for (size_t i = 0; i < vec_agg->rows.size(); ++i)
                {
                    StreamItem item;
                    item.row = std::move(vec_agg->rows[i]);
                    if (i < vec_agg->row_aggs.size())
                        item.aggs = std::move(vec_agg->row_aggs[i]);
                    POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), layout));
                    if (!keep_going)
                        break;
                }
                return layout;
            }
        }
        Batch output_layout;
        auto child = stream_rows(
            filter.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                output_layout.schemas = input_layout.schemas;
                EvalEnv env{&input_layout, &item.row, outer,
                            item.aggs.empty() ? nullptr : &item.aggs,
                            outer ? outer->depth + 1 : 0};
                POPO_ASSIGN_OR_RETURN(auto condition, eval(filter.condition, env));
                if (!as_bool(condition))
                    return true;
                return consumer(std::move(item), output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout.schemas = child->schemas;
        return output_layout;
    }
    if (node->type == PlanNodeType::Limit)
    {
        const auto & limit = static_cast<const popo::planner::LimitNode &>(*node);
        if (limit.limit_count >= 0 && limit.child && limit.child->type == PlanNodeType::Sort)
        {
            POPO_ASSIGN_OR_RETURN(
                Batch topn,
                exec_topn_sort(
                    static_cast<const popo::planner::SortNode &>(*limit.child),
                    limit.limit_count, limit.offset_count, outer));
            Batch layout;
            layout.schemas = topn.schemas;
            const bool has_aggs = !topn.row_aggs.empty();
            for (size_t i = 0; i < topn.rows.size(); ++i)
            {
                StreamItem item;
                item.row = std::move(topn.rows[i]);
                if (has_aggs)
                    item.aggs = std::move(topn.row_aggs[i]);
                POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), layout));
                if (!keep_going)
                    break;
            }
            return layout;
        }
        Batch output_layout;
        int64_t skip = std::max<int64_t>(limit.offset_count, 0);
        int64_t remaining = limit.limit_count;
        auto child = stream_rows(
            limit.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                output_layout.schemas = input_layout.schemas;
                if (skip > 0)
                {
                    --skip;
                    return true;
                }
                if (remaining == 0)
                    return false;
                POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), output_layout));
                if (remaining > 0)
                    --remaining;
                return keep_going && remaining != 0;
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout.schemas = child->schemas;
        return output_layout;
    }
    if (node->type == PlanNodeType::Project)
    {
        const auto & project = static_cast<const popo::planner::ProjectNode &>(*node);
        Batch output_layout;
        std::vector<ExprPtr> outputs;
        std::vector<size_t> star_columns;
        bool prepared = false;
        auto prepare = [&](const Batch & input_layout)
        {
            if (prepared)
                return;
            ColumnSchema cs;
            for (size_t i = 0; i < project.expressions.size(); ++i)
            {
                const auto & expr = project.expressions[i];
                std::string name = i < project.output_names.size() ? project.output_names[i] : "";
                if (expr->type == ExprType::Star)
                {
                    const auto & star = static_cast<const popo::parser::StarExpr &>(*expr);
                    size_t offset = 0;
                    for (const auto & schema : input_layout.schemas)
                    {
                        const bool include = star.table_name.empty()
                            || eq_ci(star.table_name, schema.table)
                            || (!schema.alias.empty() && eq_ci(star.table_name, schema.alias));
                        for (const auto & column : schema.columns)
                        {
                            if (include)
                            {
                                cs.columns.push_back(column);
                                outputs.push_back(nullptr);
                                star_columns.push_back(offset);
                            }
                            ++offset;
                        }
                    }
                    continue;
                }
                if (name.empty())
                    name = expr->type == ExprType::FunctionCall
                        ? agg_display_name(static_cast<const popo::parser::FunctionCallExpr &>(*expr))
                        : std::format("column{}", cs.columns.size());
                cs.columns.push_back(std::move(name));
                outputs.push_back(expr);
            }
            output_layout.schemas.push_back(std::move(cs));
            prepared = true;
        };
        auto child = stream_rows(
            project.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                prepare(input_layout);
                EvalEnv env{&input_layout, &item.row, outer,
                            item.aggs.empty() ? nullptr : &item.aggs,
                            outer ? outer->depth + 1 : 0};
                Row projected;
                projected.reserve_values(outputs.size());
                size_t star_index = 0;
                for (const auto & expr : outputs)
                {
                    if (!expr)
                        projected.append_value(item.row.values_[star_columns[star_index++]]);
                    else
                    {
                        POPO_ASSIGN_OR_RETURN(Value value, eval(expr, env));
                        projected.append_value(std::move(value));
                    }
                }
                return consumer(StreamItem{std::move(projected), {}}, output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        prepare(*child);
        return output_layout;
    }

    if (node->type == PlanNodeType::Join)
    {
        return stream_join(
            static_cast<const popo::planner::JoinNode &>(*node), outer, consumer);
    }

    POPO_ASSIGN_OR_RETURN(Batch batch, exec_batch(node, outer));
    Batch layout;
    layout.schemas = batch.schemas;
    const bool has_aggs = !batch.row_aggs.empty();
    if (batch.spilled_rows)
    {
        SpillReader reader(*batch.spilled_rows);
        for (;;)
        {
            auto row = reader.next();
            if (!row)
                return std::unexpected(row.error());
            if (!*row)
                break;
            POPO_ASSIGN_OR_RETURN(
                bool keep_going,
                consumer(StreamItem{std::move(**row), {}}, layout));
            if (!keep_going)
                break;
        }
        return layout;
    }
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        StreamItem item;
        item.row = std::move(batch.rows[i]);
        if (has_aggs)
            item.aggs = std::move(batch.row_aggs[i]);
        POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), layout));
        if (!keep_going)
            break;
    }
    return layout;
}

} // namespace popo::executor
