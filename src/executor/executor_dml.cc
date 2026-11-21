#include "executor/executor.h"

#include <format>
#include <string>
#include <vector>

#include "executor/executor_internal.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::as_bool;
using popo::function::eq_ci;
using popo::function::is_null;
using popo::type::Row;
using popo::type::Value;

popo::base::Result<void> Executor::exec_insert(const popo::planner::InsertNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return executor_error(std::format("Table '{}' has no storage", node.table_name), popo::base::Errno::InternalErr);

    const auto & schema = *qr;
    std::vector<int32_t> positions(schema->columns_.size(), -1);
    if (node.columns.empty())
    {
        for (size_t i = 0; i < positions.size(); ++i)
            positions[i] = static_cast<int32_t>(i);
    }
    else
    {
        for (size_t i = 0; i < node.columns.size(); ++i)
        {
            bool found = false;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (eq_ci(schema->columns_[j].column_name_, node.columns[i]))
                {
                    positions[j] = static_cast<int32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found)
                return executor_error(std::format("Column '{}' not found", node.columns[i]));
        }
    }

    EvalEnv env;
    for (const auto & values : node.values)
    {
        if (values.size() > (node.columns.empty() ? schema->columns_.size() : node.columns.size()))
            return executor_error("INSERT has more values than columns");
        std::vector<Value> evaluated;
        evaluated.reserve(values.size());
        for (const auto & e : values)
        {
            POPO_ASSIGN_OR_RETURN(auto value, eval(e, env));
            evaluated.push_back(std::move(value));
        }
        Row row;
        row.reserve_values(schema->columns_.size());
        for (size_t j = 0; j < schema->columns_.size(); ++j)
        {
            const int32_t pos = positions[j];
            if (pos >= 0 && static_cast<size_t>(pos) < evaluated.size())
                row.append_value(evaluated[static_cast<size_t>(pos)]);
            else if (schema->columns_[j].default_value_.has_value())
                row.append_value(*schema->columns_[j].default_value_);
            else
                row.append_value(Value());
        }
        POPO_TRY(engine_.mvcc()->stage_insert(txn_, node.table_name, row));
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_update(const popo::planner::UpdateNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return {};
    const auto & schema = *qr;

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : schema->columns_)
        cs.columns.push_back(col.column_name_);
    layout.schemas.push_back(std::move(cs));

    std::vector<Row> targets;
    {
        EvalEnv env;
        env.layout = &layout;
        for (auto & row : engine_.mvcc()->scan(txn_, node.table_name))
        {
            env.row = &row;
            bool selected = true;
            if (node.where_clause)
            {
                POPO_ASSIGN_OR_RETURN(auto condition, eval(node.where_clause, env));
                selected = as_bool(condition);
            }
            if (selected)
                targets.push_back(std::move(row));
        }
    }
    for (auto & row : targets)
    {
        EvalEnv env;
        env.layout = &layout;
        env.row = &row;
        Row updated = row;
        for (const auto & assign : node.assignments)
        {
            int32_t idx = -1;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (eq_ci(schema->columns_[j].column_name_, assign.column_name))
                {
                    idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (idx < 0)
                return executor_error(std::format("Column '{}' not found", assign.column_name));
            POPO_ASSIGN_OR_RETURN(updated.values_[static_cast<size_t>(idx)], eval(assign.value, env));
        }
        Value old_pk = schema->extract_pk(row);
        Value new_pk = schema->extract_pk(updated);
        if (!(old_pk == new_pk))
        {
            POPO_TRY(engine_.mvcc()->stage_delete(txn_, node.table_name, old_pk));
            POPO_TRY(engine_.mvcc()->stage_insert(txn_, node.table_name, updated));
        }
        else
        {
            POPO_TRY(engine_.mvcc()->stage_update(txn_, node.table_name, old_pk, updated));
        }
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_delete(const popo::planner::DeleteNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return {};

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : (*qr)->columns_)
        cs.columns.push_back(col.column_name_);
    layout.schemas.push_back(std::move(cs));

    std::vector<Value> targets;
    {
        EvalEnv env;
        env.layout = &layout;
        for (auto & row : engine_.mvcc()->scan(txn_, node.table_name))
        {
            env.row = &row;
            bool selected = true;
            if (node.where_clause)
            {
                POPO_ASSIGN_OR_RETURN(auto condition, eval(node.where_clause, env));
                selected = as_bool(condition);
            }
            if (selected)
                targets.push_back((*qr)->extract_pk(row));
        }
    }
    for (auto & pk : targets)
    {
        POPO_TRY(engine_.mvcc()->stage_delete(txn_, node.table_name, pk));
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_alter_table(const popo::planner::AlterTableNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    if (node.action == popo::parser::AlterAction::NoOp)
    {
        result_.message = "Table altered";
        return {};
    }
    const auto old_schema = **qr;

    std::vector<Row> rows = engine_.mvcc()->scan(txn_, node.table_name);
    auto columns = old_schema.columns_;
    std::string new_name = node.table_name;

    switch (node.action)
    {
        case popo::parser::AlterAction::AddColumn: {
            for (const auto & col : columns)
            {
                if (eq_ci(col.column_name_, node.column.name))
                    return executor_error(std::format("Column '{}' already exists", node.column.name));
            }
            popo::catalog::Column col{};
            col.column_name_ = node.column.name;
            col.data_type_ = node.column.data_type;
            col.type_len_ = node.column.type_len;
            col.is_nullable_ = node.column.is_nullable;
            col.default_value_ = node.column.default_value;
            columns.push_back(col);
            Value fill = node.column.default_value.value_or(Value());
            for (auto & row : rows)
                row.append_value(fill);
            break;
        }
        case popo::parser::AlterAction::DropColumn: {
            int32_t idx = -1;
            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (eq_ci(columns[i].column_name_, node.target_name))
                {
                    idx = static_cast<int32_t>(i);
                    break;
                }
            }
            if (idx < 0)
                return executor_error(std::format("Column '{}' not found", node.target_name));
            if (columns[static_cast<size_t>(idx)].is_primary_key_)
                return executor_error("cannot drop the primary key column");
            if (columns.size() == 1)
                return executor_error("cannot drop the last column");
            columns.erase(columns.begin() + idx);
            for (auto & row : rows)
                row.values_.erase(row.values_.begin() + idx);
            break;
        }
        case popo::parser::AlterAction::RenameTable: {
            auto existing = schema_mgr_.query_table(node.target_name);
            if (existing && *existing)
                return executor_error(
                    std::format("Table '{}' already exists", node.target_name), popo::base::Errno::TableAlreadyExists);
            new_name = node.target_name;
            break;
        }
        case popo::parser::AlterAction::AddPrimaryKey: {
            for (const auto & col : columns)
                if (col.is_primary_key_)
                    return executor_error("table already has a primary key");
            for (const auto & kname : node.key_columns)
            {
                int32_t idx = -1;
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    if (eq_ci(columns[i].column_name_, kname))
                    {
                        idx = static_cast<int32_t>(i);
                        break;
                    }
                }
                if (idx < 0)
                    return executor_error(std::format("Column '{}' not found", kname));
                for (const auto & row : rows)
                {
                    if (static_cast<size_t>(idx) >= row.values_.size() || is_null(row.values_[static_cast<size_t>(idx)]))
                        return executor_error("primary key column cannot contain NULL");
                }
                columns[static_cast<size_t>(idx)].is_primary_key_ = true;
                columns[static_cast<size_t>(idx)].is_nullable_ = false;
            }
            break;
        }
        case popo::parser::AlterAction::NoOp:
            break;
    }

    POPO_TRY(rebuild_table(node.table_name, columns, rows, new_name));
    result_.message = "Table altered";
    return {};
}

popo::base::Result<void> Executor::rebuild_table(
    const std::string & table_name, const std::vector<popo::catalog::Column> & new_columns, const std::vector<Row> & rows,
    const std::string & new_name)
{
    auto qr = schema_mgr_.query_table(table_name);
    if (!qr || !*qr)
        return executor_error(std::format("Table '{}' not found", table_name), popo::base::Errno::TableNotFound);
    const auto old_schema = **qr;

    if (new_name != table_name)
    {
        std::vector<popo::catalog::Column> renamed = new_columns;
        auto created = schema_mgr_.create_table(new_name, renamed);
        if (!created)
            return executor_error(
                "failed to create renamed table: " + new_name, popo::base::Errno::TableAlreadyExists);
        auto del = schema_mgr_.delete_table(table_name);
        if (!del)
            return std::unexpected(del.error());
    }
    else
    {
        auto modified = schema_mgr_.modify_table(table_name, new_columns);
        if (!modified)
            return std::unexpected(modified.error());
    }

    engine_.drop_table(table_name);
    auto new_qr = schema_mgr_.query_table(new_name);
    if (!new_qr || !*new_qr)
        return executor_error("schema missing after ALTER TABLE", popo::base::Errno::InternalErr);
    POPO_TRY(engine_.create_table(new_name, **new_qr));
    auto * heap = engine_.get_table_heap(new_name);
    if (!heap)
        return executor_error("storage missing after ALTER TABLE", popo::base::Errno::InternalErr);

    for (const auto & index : old_schema.indexes_)
    {
        std::vector<int32_t> mapped;
        bool valid = true;
        for (int32_t old_idx : index.index_cols_)
        {
            int32_t new_idx = -1;
            for (size_t j = 0; j < new_columns.size(); ++j)
            {
                if (old_idx >= 0 && static_cast<size_t>(old_idx) < old_schema.columns_.size()
                    && eq_ci(new_columns[j].column_name_, old_schema.columns_[static_cast<size_t>(old_idx)].column_name_))
                {
                    new_idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (new_idx < 0)
            {
                valid = false;
                break;
            }
            mapped.push_back(new_idx);
        }
        if (new_name == table_name)
        {
            auto del = schema_mgr_.delete_index(table_name, index.index_name_);
            if (!del)
                return std::unexpected(del.error());
        }
        if (!valid || mapped.size() != 1)
            continue;
        auto created = schema_mgr_.create_index(new_name, index.index_name_, mapped, index.is_unique_);
        if (!created)
            return std::unexpected(created.error());
        POPO_TRY(engine_.create_index(new_name, index.index_name_, mapped[0], index.is_unique_));
    }

    for (const auto & row : rows)
        POPO_TRY(engine_.mvcc()->stage_insert(txn_, new_name, row));
    return {};
}

popo::base::Result<void> Executor::exec_create_view(const popo::planner::CreateViewNode & node)
{
    auto created = schema_mgr_.create_view(node.name, node.query, node.column_names, node.or_replace);
    if (!created)
        return std::unexpected(created.error());
    result_.message = "View created";
    return {};
}

popo::base::Result<void> Executor::exec_drop_view(const popo::planner::DropViewNode & node)
{
    auto dropped = schema_mgr_.drop_view(node.name, node.if_exists);
    if (!dropped)
        return std::unexpected(dropped.error());
    result_.message = "View dropped";
    return {};
}

} // namespace popo::executor
