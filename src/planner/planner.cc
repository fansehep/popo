#include "planner/planner.h"
#include <format>
#include <stdexcept>

namespace popo::planner
{

using popo::base::Error;

namespace
{

// Check if a single expression contains an aggregate function call
bool HasAggregate(const popo::parser::ExprPtr & expr)
{
    if (!expr)
        return false;
    if (expr->type == popo::parser::ExprType::FunctionCall)
    {
        return true;
    }
    if (expr->type == popo::parser::ExprType::BinaryOp)
    {
        const auto & bin = dynamic_cast<const popo::parser::BinaryOpExpr &>(*expr);
        return HasAggregate(bin.left) || HasAggregate(bin.right);
    }
    if (expr->type == popo::parser::ExprType::UnaryOp)
    {
        const auto & un = dynamic_cast<const popo::parser::UnaryOpExpr &>(*expr);
        return HasAggregate(un.operand);
    }
    return false;
}

// Check if any expression in a list contains an aggregate
bool HasAggregates(const std::vector<popo::parser::ExprPtr> & exprs)
{
    for (const auto & e : exprs)
    {
        if (HasAggregate(e))
            return true;
    }
    return false;
}

// Collect all aggregate function calls from an expression tree
void CollectAggregates(const popo::parser::ExprPtr & expr, std::vector<popo::parser::ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == popo::parser::ExprType::FunctionCall)
    {
        out.push_back(expr);
        return;
    }
    if (expr->type == popo::parser::ExprType::BinaryOp)
    {
        const auto & bin = dynamic_cast<const popo::parser::BinaryOpExpr &>(*expr);
        CollectAggregates(bin.left, out);
        CollectAggregates(bin.right, out);
    }
    if (expr->type == popo::parser::ExprType::UnaryOp)
    {
        const auto & un = dynamic_cast<const popo::parser::UnaryOpExpr &>(*expr);
        CollectAggregates(un.operand, out);
    }
}

// Extract all aggregate expressions from a select list
std::vector<popo::parser::ExprPtr> ExtractAggregates(const std::vector<popo::parser::ExprPtr> & select_list)
{
    std::vector<popo::parser::ExprPtr> result;
    for (const auto & e : select_list)
    {
        CollectAggregates(e, result);
    }
    return result;
}

// Derive a display name from an expression
std::string GetExprName(const popo::parser::ExprPtr & expr)
{
    if (!expr)
        return "";
    if (expr->type == popo::parser::ExprType::ColumnRef)
    {
        return dynamic_cast<const popo::parser::ColumnRefExpr &>(*expr).column_name;
    }
    return "";
}

} // anonymous namespace

Planner::Planner(popo::catalog::SchemaManager & schema_mgr) : schema_mgr_(schema_mgr)
{
}

popo::base::Result<PlanNodePtr> Planner::plan(const popo::parser::StmtPtr & stmt)
{
    try
    {
        switch (stmt->type)
        {
            case popo::parser::StmtType::Select:
                return plan_select(dynamic_cast<const popo::parser::SelectStatement &>(*stmt));
            case popo::parser::StmtType::Insert:
                return plan_insert(dynamic_cast<const popo::parser::InsertStatement &>(*stmt));
            case popo::parser::StmtType::CreateTable:
                return plan_create_table(dynamic_cast<const popo::parser::CreateTableStatement &>(*stmt));
            case popo::parser::StmtType::CreateIndex:
                return plan_create_index(dynamic_cast<const popo::parser::CreateIndexStatement &>(*stmt));
            case popo::parser::StmtType::Update:
                return plan_update(dynamic_cast<const popo::parser::UpdateStatement &>(*stmt));
            case popo::parser::StmtType::Delete:
                return plan_delete(dynamic_cast<const popo::parser::DeleteStatement &>(*stmt));
            case popo::parser::StmtType::DropTable:
                return plan_drop_table(dynamic_cast<const popo::parser::DropTableStatement &>(*stmt));
            case popo::parser::StmtType::ShowTables:
                return plan_show_tables(dynamic_cast<const popo::parser::ShowTablesStatement &>(*stmt));
            case popo::parser::StmtType::Describe:
                return plan_describe(dynamic_cast<const popo::parser::DescribeStatement &>(*stmt));
            case popo::parser::StmtType::ShowDatabases:
                return plan_show_databases(dynamic_cast<const popo::parser::ShowDatabasesStatement &>(*stmt));
            case popo::parser::StmtType::SetVar:
                return plan_set_var(dynamic_cast<const popo::parser::SetVarStatement &>(*stmt));
            case popo::parser::StmtType::ShowVariables:
                return plan_show_variables(dynamic_cast<const popo::parser::ShowVariablesStatement &>(*stmt));
            case popo::parser::StmtType::ShowStatus:
                return plan_show_status(dynamic_cast<const popo::parser::ShowStatusStatement &>(*stmt));
            case popo::parser::StmtType::SelectDatabase:
                return plan_select_database(dynamic_cast<const popo::parser::SelectDatabaseStatement &>(*stmt));
            case popo::parser::StmtType::SelectSystemVar:
                return plan_select_system_var(dynamic_cast<const popo::parser::SelectSystemVarStatement &>(*stmt));
        }
    }
    catch (const std::exception & e)
    {
        return std::unexpected(Error{popo::base::Unknown, e.what()});
    }
    return std::unexpected(Error{popo::base::Unknown, "Unknown statement type"});
}

PlanNodePtr Planner::plan_select(const popo::parser::SelectStatement & stmt)
{
    // FROM (Scan/Join)
    PlanNodePtr plan = nullptr;
    if (stmt.from)
    {
        plan = plan_table_ref(stmt.from);
    }

    // WHERE (Filter)
    if (stmt.where)
    {
        auto filter = std::make_shared<FilterNode>();
        filter->child = plan;
        filter->condition = stmt.where;
        plan = filter;
    }

    // GROUP BY + aggregates (Agg)
    bool has_agg = !stmt.group_by.empty() || HasAggregates(stmt.select_list);
    if (has_agg)
    {
        auto agg = std::make_shared<AggNode>();
        agg->child = plan;
        agg->group_by = stmt.group_by;
        agg->agg_exprs = ExtractAggregates(stmt.select_list);
        for (const auto & expr : stmt.select_list)
        {
            agg->output_names.push_back(GetExprName(expr));
        }
        plan = agg;
    }

    // HAVING (Filter)
    if (stmt.having)
    {
        auto filter = std::make_shared<FilterNode>();
        filter->child = plan;
        filter->condition = stmt.having;
        plan = filter;
    }

    // ORDER BY (Sort)
    if (!stmt.order_by.empty())
    {
        auto sort = std::make_shared<SortNode>();
        sort->child = plan;
        sort->order_by = stmt.order_by;
        plan = sort;
    }

    // LIMIT (Limit)
    if (stmt.limit.has_value())
    {
        auto limit = std::make_shared<LimitNode>();
        limit->child = plan;
        limit->limit_count = stmt.limit.value();
        plan = limit;
    }

    // SELECT list (Project)
    auto project = std::make_shared<ProjectNode>();
    project->child = plan;
    project->expressions = stmt.select_list;
    for (const auto & expr : stmt.select_list)
    {
        project->output_names.push_back(GetExprName(expr));
    }
    plan = project;

    return plan;
}

PlanNodePtr Planner::plan_table_ref(const popo::parser::TableRefPtr & ref)
{
    if (!ref)
        return nullptr;

    if (ref->type == popo::parser::TableRefType::Simple)
    {
        const auto & simple = dynamic_cast<const popo::parser::SimpleTableRef &>(*ref);
        auto scan = std::make_shared<ScanNode>();
        scan->table_name = simple.table_name;
        scan->alias = simple.alias;
        resolve_scan_columns(*scan);
        return scan;
    }

    if (ref->type == popo::parser::TableRefType::Join)
    {
        const auto & join = static_cast<const popo::parser::JoinTableRef &>(*ref);
        auto node = std::make_shared<JoinNode>();
        node->join_type = join.join_type;
        node->left = plan_table_ref(join.left);
        node->right = plan_table_ref(join.right);
        node->condition = join.condition;
        return node;
    }

    return nullptr;
}

void Planner::resolve_scan_columns(ScanNode & scan)
{
    auto qr = schema_mgr_.query_table(scan.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", scan.table_name));
    }
    auto & schema = *qr;
    for (const auto & col : schema->columns_)
    {
        scan.column_names.push_back(col.column_name_);
        scan.column_types.push_back(col.data_type_);
    }
}

PlanNodePtr Planner::plan_insert(const popo::parser::InsertStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<InsertNode>();
    node->table_name = stmt.table_name;
    node->columns = stmt.columns;
    node->values = stmt.values;
    return node;
}

PlanNodePtr Planner::plan_create_table(const popo::parser::CreateTableStatement & stmt)
{
    auto node = std::make_shared<CreateTableNode>();
    node->table_name = stmt.table_name;
    node->columns = stmt.columns;
    return node;
}

PlanNodePtr Planner::plan_create_index(const popo::parser::CreateIndexStatement & stmt)
{
    if (stmt.columns.size() != 1)
    {
        throw std::runtime_error("only a single-column secondary index is supported");
    }
    auto node = std::make_shared<CreateIndexNode>();
    node->unique = stmt.unique;
    node->index_name = stmt.index_name;
    node->table_name = stmt.table_name;
    node->column_name = stmt.columns[0];
    return node;
}

PlanNodePtr Planner::plan_update(const popo::parser::UpdateStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<UpdateNode>();
    node->table_name = stmt.table_name;
    node->assignments = stmt.assignments;
    node->where_clause = stmt.where;
    return node;
}

PlanNodePtr Planner::plan_delete(const popo::parser::DeleteStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<DeleteNode>();
    node->table_name = stmt.table_name;
    node->where_clause = stmt.where;
    return node;
}

PlanNodePtr Planner::plan_drop_table(const popo::parser::DropTableStatement & stmt)
{
    auto node = std::make_shared<DropTableNode>();
    node->table_name = stmt.table_name;
    node->if_exists = stmt.if_exists;
    return node;
}

PlanNodePtr Planner::plan_show_tables(const popo::parser::ShowTablesStatement & /*stmt*/)
{
    return std::make_shared<ShowTablesNode>();
}

PlanNodePtr Planner::plan_describe(const popo::parser::DescribeStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' doesn't exist", stmt.table_name));
    }
    auto node = std::make_shared<DescribeNode>();
    node->table_name = stmt.table_name;
    return node;
}

PlanNodePtr Planner::plan_show_databases(const popo::parser::ShowDatabasesStatement & /*stmt*/)
{
    return std::make_shared<ShowDatabasesNode>();
}

PlanNodePtr Planner::plan_set_var(const popo::parser::SetVarStatement & /*stmt*/)
{
    return std::make_shared<SetVarNode>();
}

PlanNodePtr Planner::plan_show_variables(const popo::parser::ShowVariablesStatement & /*stmt*/)
{
    return std::make_shared<ShowVariablesNode>();
}

PlanNodePtr Planner::plan_show_status(const popo::parser::ShowStatusStatement & /*stmt*/)
{
    return std::make_shared<ShowStatusNode>();
}

PlanNodePtr Planner::plan_select_database(const popo::parser::SelectDatabaseStatement & /*stmt*/)
{
    return std::make_shared<SelectDatabaseNode>();
}

PlanNodePtr Planner::plan_select_system_var(const popo::parser::SelectSystemVarStatement & stmt)
{
    auto node = std::make_shared<SelectSystemVarNode>();
    node->var_name = stmt.var_name;
    return node;
}

} // namespace popo::planner
