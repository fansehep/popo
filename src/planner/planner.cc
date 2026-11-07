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
    switch (expr->type)
    {
        case popo::parser::ExprType::FunctionCall:
            return true;
        case popo::parser::ExprType::BinaryOp: {
            const auto & bin = dynamic_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return HasAggregate(bin.left) || HasAggregate(bin.right);
        }
        case popo::parser::ExprType::UnaryOp:
            return HasAggregate(dynamic_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case popo::parser::ExprType::Alias:
            return HasAggregate(dynamic_cast<const popo::parser::AliasExpr &>(*expr).expr);
        case popo::parser::ExprType::In: {
            const auto & in = dynamic_cast<const popo::parser::InExpr &>(*expr);
            if (HasAggregate(in.operand))
                return true;
            for (const auto & e : in.list)
            {
                if (HasAggregate(e))
                    return true;
            }
            return false;
        }
        case popo::parser::ExprType::Between: {
            const auto & b = dynamic_cast<const popo::parser::BetweenExpr &>(*expr);
            return HasAggregate(b.operand) || HasAggregate(b.low) || HasAggregate(b.high);
        }
        case popo::parser::ExprType::Like: {
            const auto & l = dynamic_cast<const popo::parser::LikeExpr &>(*expr);
            return HasAggregate(l.operand) || HasAggregate(l.pattern);
        }
        case popo::parser::ExprType::IsNull:
            return HasAggregate(dynamic_cast<const popo::parser::IsNullExpr &>(*expr).operand);
        case popo::parser::ExprType::Case: {
            const auto & c = dynamic_cast<const popo::parser::CaseExpr &>(*expr);
            if (HasAggregate(c.operand) || HasAggregate(c.else_expr))
                return true;
            for (const auto & [cond, result] : c.whens)
            {
                if (HasAggregate(cond) || HasAggregate(result))
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
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
    switch (expr->type)
    {
        case popo::parser::ExprType::FunctionCall:
            out.push_back(expr);
            return;
        case popo::parser::ExprType::BinaryOp: {
            const auto & bin = dynamic_cast<const popo::parser::BinaryOpExpr &>(*expr);
            CollectAggregates(bin.left, out);
            CollectAggregates(bin.right, out);
            return;
        }
        case popo::parser::ExprType::UnaryOp:
            CollectAggregates(dynamic_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out);
            return;
        case popo::parser::ExprType::Alias:
            CollectAggregates(dynamic_cast<const popo::parser::AliasExpr &>(*expr).expr, out);
            return;
        case popo::parser::ExprType::In: {
            const auto & in = dynamic_cast<const popo::parser::InExpr &>(*expr);
            CollectAggregates(in.operand, out);
            for (const auto & e : in.list)
            {
                CollectAggregates(e, out);
            }
            return;
        }
        case popo::parser::ExprType::Between: {
            const auto & b = dynamic_cast<const popo::parser::BetweenExpr &>(*expr);
            CollectAggregates(b.operand, out);
            CollectAggregates(b.low, out);
            CollectAggregates(b.high, out);
            return;
        }
        case popo::parser::ExprType::Like: {
            const auto & l = dynamic_cast<const popo::parser::LikeExpr &>(*expr);
            CollectAggregates(l.operand, out);
            CollectAggregates(l.pattern, out);
            return;
        }
        case popo::parser::ExprType::IsNull:
            CollectAggregates(dynamic_cast<const popo::parser::IsNullExpr &>(*expr).operand, out);
            return;
        case popo::parser::ExprType::Case: {
            const auto & c = dynamic_cast<const popo::parser::CaseExpr &>(*expr);
            CollectAggregates(c.operand, out);
            for (const auto & [cond, result] : c.whens)
            {
                CollectAggregates(cond, out);
                CollectAggregates(result, out);
            }
            CollectAggregates(c.else_expr, out);
            return;
        }
        default:
            return;
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
    if (expr->type == popo::parser::ExprType::Alias)
    {
        return dynamic_cast<const popo::parser::AliasExpr &>(*expr).alias;
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
        cte_plans_.clear();
        cte_recursive_.clear();
        expanding_views_.clear();
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
            case popo::parser::StmtType::AlterTable:
                return plan_alter_table(dynamic_cast<const popo::parser::AlterTableStatement &>(*stmt));
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
            case popo::parser::StmtType::CreateView:
                return plan_create_view(dynamic_cast<const popo::parser::CreateViewStatement &>(*stmt));
            case popo::parser::StmtType::DropView:
                return plan_drop_view(dynamic_cast<const popo::parser::DropViewStatement &>(*stmt));
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
    for (const auto & cte : stmt.with_clause)
    {
        cte_recursive_[cte.name] = cte.recursive;
        if (cte.recursive)
        {
            auto self = std::make_shared<RecursiveScanNode>();
            self->cte_name = cte.name;
            cte_plans_[cte.name] = self;
            auto body = plan_select(*cte.query);
            cte_plans_[cte.name] = body;
        }
        else
        {
            cte_plans_[cte.name] = plan_select(*cte.query);
        }
    }

    if (stmt.set_ops.empty())
    {
        return plan_select_body(stmt);
    }

    // INTERSECT and EXCEPT bind tighter than UNION. Trailing ORDER BY / LIMIT apply to the combination.
    auto left_core = stmt;
    left_core.order_by.clear();
    left_core.limit.reset();
    left_core.offset.reset();
    left_core.set_ops.clear();
    left_core.with_clause.clear();

    struct Piece
    {
        popo::parser::SetOpKind kind = popo::parser::SetOpKind::Union;
        bool is_all = false;
        PlanNodePtr plan;
    };
    std::vector<Piece> pieces;
    pieces.push_back(Piece{popo::parser::SetOpKind::Union, false, plan_select_body(left_core)});
    for (const auto & term : stmt.set_ops)
    {
        pieces.push_back(Piece{term.kind, term.is_all, plan_select_body(*term.query)});
    }

    std::vector<Piece> union_pieces;
    PlanNodePtr current = pieces[0].plan;
    for (size_t i = 1; i < pieces.size(); ++i)
    {
        if (pieces[i].kind == popo::parser::SetOpKind::Union)
        {
            union_pieces.push_back(Piece{popo::parser::SetOpKind::Union, pieces[i].is_all, current});
            current = pieces[i].plan;
        }
        else
        {
            auto op = std::make_shared<SetOpNode>();
            op->left = current;
            op->right = pieces[i].plan;
            op->kind = pieces[i].kind;
            op->is_all = pieces[i].is_all;
            current = op;
        }
    }
    union_pieces.push_back(Piece{popo::parser::SetOpKind::Union, false, current});
    PlanNodePtr plan = union_pieces[0].plan;
    for (size_t i = 1; i < union_pieces.size(); ++i)
    {
        auto op = std::make_shared<SetOpNode>();
        op->left = plan;
        op->right = union_pieces[i].plan;
        op->kind = popo::parser::SetOpKind::Union;
        op->is_all = union_pieces[i - 1].is_all;
        plan = op;
    }

    if (!stmt.order_by.empty())
    {
        auto sort = std::make_shared<SortNode>();
        sort->child = plan;
        sort->order_by = stmt.order_by;
        plan = sort;
    }
    if (stmt.limit.has_value() || stmt.offset.has_value())
    {
        auto limit = std::make_shared<LimitNode>();
        limit->child = plan;
        limit->limit_count = stmt.limit.value_or(-1);
        limit->offset_count = stmt.offset.value_or(0);
        plan = limit;
    }
    return plan;
}

PlanNodePtr Planner::plan_select_body(const popo::parser::SelectStatement & stmt)
{
    // FROM (Scan/Join), or a single empty row when FROM is omitted
    PlanNodePtr plan = nullptr;
    if (stmt.from)
    {
        plan = plan_table_ref(stmt.from);
    }
    else
    {
        plan = std::make_shared<DualNode>();
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
    bool has_agg = !stmt.group_by.empty() || HasAggregates(stmt.select_list) || (stmt.having && HasAggregate(stmt.having));
    if (has_agg)
    {
        auto agg = std::make_shared<AggNode>();
        agg->child = plan;
        agg->group_by = stmt.group_by;
        agg->agg_exprs = ExtractAggregates(stmt.select_list);
        CollectAggregates(stmt.having, agg->agg_exprs);
        for (const auto & item : stmt.order_by)
        {
            CollectAggregates(item.expr, agg->agg_exprs);
        }
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

    // Window functions run after WHERE / GROUP BY / HAVING and before ORDER BY / LIMIT.
    std::vector<popo::parser::ExprPtr> windows;
    auto collect_windows = [&](const popo::parser::ExprPtr & expr, auto & self) -> void
    {
        if (!expr)
            return;
        if (expr->type == popo::parser::ExprType::Alias)
        {
            self(static_cast<const popo::parser::AliasExpr &>(*expr).expr, self);
            return;
        }
        if (expr->type == popo::parser::ExprType::Window)
        {
            windows.push_back(expr);
        }
    };
    for (const auto & expr : stmt.select_list)
    {
        collect_windows(expr, collect_windows);
    }
    if (!windows.empty())
    {
        auto window = std::make_shared<WindowNode>();
        window->child = plan;
        window->windows = std::move(windows);
        plan = window;
    }

    // ORDER BY (Sort)
    if (!stmt.order_by.empty())
    {
        auto sort = std::make_shared<SortNode>();
        sort->child = plan;
        sort->order_by = stmt.order_by;
        plan = sort;
    }

    // LIMIT / OFFSET (Limit)
    if (stmt.limit.has_value() || stmt.offset.has_value())
    {
        auto limit = std::make_shared<LimitNode>();
        limit->child = plan;
        limit->limit_count = stmt.limit.value_or(-1);
        limit->offset_count = stmt.offset.value_or(0);
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

    // DISTINCT applies to projected rows
    if (stmt.distinct)
    {
        auto distinct = std::make_shared<DistinctNode>();
        distinct->child = plan;
        plan = distinct;
    }

    return plan;
}

PlanNodePtr Planner::plan_table_ref(const popo::parser::TableRefPtr & ref)
{
    if (!ref)
        return nullptr;

    if (ref->type == popo::parser::TableRefType::Simple)
    {
        const auto & simple = dynamic_cast<const popo::parser::SimpleTableRef &>(*ref);
        auto cte = cte_plans_.find(simple.table_name);
        if (cte != cte_plans_.end())
        {
            if (cte->second && cte->second->type == PlanNodeType::RecursiveScan)
            {
                return cte->second;
            }
            auto scan = std::make_shared<SubqueryScanNode>();
            scan->alias = simple.alias.empty() ? simple.table_name : simple.alias;
            scan->cte_name = simple.table_name;
            scan->recursive = cte_recursive_[simple.table_name];
            scan->column_names = plan_output_names(cte->second);
            scan->child = cte->second;
            return scan;
        }
        if (auto view = schema_mgr_.find_view(simple.table_name))
        {
            if (expanding_views_.contains(simple.table_name))
            {
                throw std::runtime_error("view cycle: " + simple.table_name);
            }
            expanding_views_.insert(simple.table_name);
            const auto & body = dynamic_cast<const popo::parser::SelectStatement &>(*view);
            auto scan = std::make_shared<SubqueryScanNode>();
            scan->alias = simple.alias.empty() ? simple.table_name : simple.alias;
            scan->child = plan_select(body);
            scan->column_names = plan_output_names(scan->child);
            expanding_views_.erase(simple.table_name);
            return scan;
        }
        auto scan = std::make_shared<ScanNode>();
        scan->table_name = simple.table_name;
        scan->alias = simple.alias;
        resolve_scan_columns(*scan);
        return scan;
    }

    if (ref->type == popo::parser::TableRefType::Derived)
    {
        const auto & derived = dynamic_cast<const popo::parser::DerivedTableRef &>(*ref);
        const auto & query = dynamic_cast<const popo::parser::SelectStatement &>(*derived.query);
        auto scan = std::make_shared<SubqueryScanNode>();
        scan->alias = derived.alias;
        scan->child = plan_select(query);
        scan->column_names = plan_output_names(scan->child);
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

PlanNodePtr Planner::plan_alter_table(const popo::parser::AlterTableStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        throw std::runtime_error(std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<AlterTableNode>();
    node->table_name = stmt.table_name;
    node->action = stmt.action;
    node->column = stmt.column;
    node->target_name = stmt.target_name;
    return node;
}

std::vector<std::string> Planner::plan_output_names(const PlanNodePtr & node)
{
    if (!node)
        return {};
    switch (node->type)
    {
        case PlanNodeType::Scan:
            return static_cast<const ScanNode &>(*node).column_names;
        case PlanNodeType::SubqueryScan:
            return static_cast<const SubqueryScanNode &>(*node).column_names;
        case PlanNodeType::Project: {
            const auto & project = static_cast<const ProjectNode &>(*node);
            std::vector<std::string> names;
            auto child_names = plan_output_names(project.child);
            for (size_t i = 0; i < project.expressions.size(); ++i)
            {
                const auto & expr = project.expressions[i];
                if (expr->type == popo::parser::ExprType::Star)
                {
                    names.insert(names.end(), child_names.begin(), child_names.end());
                }
                else if (i < project.output_names.size() && !project.output_names[i].empty())
                {
                    names.push_back(project.output_names[i]);
                }
                else if (expr->type == popo::parser::ExprType::FunctionCall)
                {
                    const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
                    static const char * kAggNames[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
                    std::string name = kAggNames[static_cast<int32_t>(func.func)];
                    name += "(";
                    if (!func.args.empty() && func.args[0]->type == popo::parser::ExprType::ColumnRef)
                    {
                        name += static_cast<const popo::parser::ColumnRefExpr &>(*func.args[0]).column_name;
                    }
                    else
                    {
                        name += "*";
                    }
                    name += ")";
                    names.push_back(name);
                }
                else
                {
                    names.push_back(std::format("column{}", names.size()));
                }
            }
            return names;
        }
        case PlanNodeType::Filter:
            return plan_output_names(static_cast<const FilterNode &>(*node).child);
        case PlanNodeType::Sort:
            return plan_output_names(static_cast<const SortNode &>(*node).child);
        case PlanNodeType::Limit:
            return plan_output_names(static_cast<const LimitNode &>(*node).child);
        case PlanNodeType::Distinct:
            return plan_output_names(static_cast<const DistinctNode &>(*node).child);
        case PlanNodeType::Agg:
            return plan_output_names(static_cast<const AggNode &>(*node).child);
        case PlanNodeType::SetOp:
            return plan_output_names(static_cast<const SetOpNode &>(*node).left);
        case PlanNodeType::Join: {
            const auto & join = static_cast<const JoinNode &>(*node);
            auto names = plan_output_names(join.left);
            auto right = plan_output_names(join.right);
            names.insert(names.end(), right.begin(), right.end());
            return names;
        }
        default:
            return {};
    }
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

PlanNodePtr Planner::plan_create_view(const popo::parser::CreateViewStatement & stmt)
{
    if (schema_mgr_.find_view(stmt.name))
    {
        throw std::runtime_error("view already exists: " + stmt.name);
    }
    auto existing = schema_mgr_.query_table(stmt.name);
    if (existing && *existing)
    {
        throw std::runtime_error("name already exists: " + stmt.name);
    }
    auto node = std::make_shared<CreateViewNode>();
    node->name = stmt.name;
    node->query = stmt.query;
    return node;
}

PlanNodePtr Planner::plan_drop_view(const popo::parser::DropViewStatement & stmt)
{
    auto node = std::make_shared<DropViewNode>();
    node->name = stmt.name;
    return node;
}

} // namespace popo::planner
