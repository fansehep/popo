#include "planner/planner.h"
#include <format>

namespace popo::planner
{

using popo::base::Errno;
using popo::base::Error;
using popo::base::make_error;
using popo::base::Result;

namespace
{

bool HasAggregate(const popo::parser::ExprPtr & expr)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case popo::parser::ExprType::FunctionCall:
            return true;
        case popo::parser::ExprType::BinaryOp: {
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return HasAggregate(bin.left) || HasAggregate(bin.right);
        }
        case popo::parser::ExprType::UnaryOp:
            return HasAggregate(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case popo::parser::ExprType::Alias:
            return HasAggregate(static_cast<const popo::parser::AliasExpr &>(*expr).expr);
        case popo::parser::ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
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
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            return HasAggregate(b.operand) || HasAggregate(b.low) || HasAggregate(b.high);
        }
        case popo::parser::ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            return HasAggregate(l.operand) || HasAggregate(l.pattern);
        }
        case popo::parser::ExprType::IsNull:
            return HasAggregate(static_cast<const popo::parser::IsNullExpr &>(*expr).operand);
        case popo::parser::ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            if (HasAggregate(c.operand) || HasAggregate(c.else_expr))
                return true;
            for (const auto & [cond, result] : c.whens)
            {
                if (HasAggregate(cond) || HasAggregate(result))
                    return true;
            }
            return false;
        }
        case popo::parser::ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & arg : sc.args)
            {
                if (HasAggregate(arg))
                    return true;
            }
            return false;
        }
        case popo::parser::ExprType::Cast:
            return HasAggregate(static_cast<const popo::parser::CastExpr &>(*expr).operand);
        case popo::parser::ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            return HasAggregate(d.left) || HasAggregate(d.right);
        }
        default:
            return false;
    }
}

bool HasAggregates(const std::vector<popo::parser::ExprPtr> & exprs)
{
    for (const auto & e : exprs)
    {
        if (HasAggregate(e))
            return true;
    }
    return false;
}

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
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            CollectAggregates(bin.left, out);
            CollectAggregates(bin.right, out);
            return;
        }
        case popo::parser::ExprType::UnaryOp:
            CollectAggregates(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out);
            return;
        case popo::parser::ExprType::Alias:
            CollectAggregates(static_cast<const popo::parser::AliasExpr &>(*expr).expr, out);
            return;
        case popo::parser::ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            CollectAggregates(in.operand, out);
            for (const auto & e : in.list)
            {
                CollectAggregates(e, out);
            }
            return;
        }
        case popo::parser::ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            CollectAggregates(b.operand, out);
            CollectAggregates(b.low, out);
            CollectAggregates(b.high, out);
            return;
        }
        case popo::parser::ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            CollectAggregates(l.operand, out);
            CollectAggregates(l.pattern, out);
            return;
        }
        case popo::parser::ExprType::IsNull:
            CollectAggregates(static_cast<const popo::parser::IsNullExpr &>(*expr).operand, out);
            return;
        case popo::parser::ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            CollectAggregates(c.operand, out);
            for (const auto & [cond, result] : c.whens)
            {
                CollectAggregates(cond, out);
                CollectAggregates(result, out);
            }
            CollectAggregates(c.else_expr, out);
            return;
        }
        case popo::parser::ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & arg : sc.args)
            {
                CollectAggregates(arg, out);
            }
            return;
        }
        case popo::parser::ExprType::Cast:
            CollectAggregates(static_cast<const popo::parser::CastExpr &>(*expr).operand, out);
            return;
        case popo::parser::ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            CollectAggregates(d.left, out);
            CollectAggregates(d.right, out);
            return;
        }
        default:
            return;
    }
}

std::vector<popo::parser::ExprPtr> ExtractAggregates(const std::vector<popo::parser::ExprPtr> & select_list)
{
    std::vector<popo::parser::ExprPtr> result;
    for (const auto & e : select_list)
    {
        CollectAggregates(e, result);
    }
    return result;
}

std::string GetExprName(const popo::parser::ExprPtr & expr)
{
    if (!expr)
        return "";
    if (expr->type == popo::parser::ExprType::ColumnRef)
    {
        return static_cast<const popo::parser::ColumnRefExpr &>(*expr).column_name;
    }
    if (expr->type == popo::parser::ExprType::Alias)
    {
        return static_cast<const popo::parser::AliasExpr &>(*expr).alias;
    }
    return "";
}

} // namespace

Planner::Planner(popo::catalog::SchemaManager & schema_mgr) : schema_mgr_(schema_mgr)
{
}

Result<PlanNodePtr> Planner::plan(const popo::parser::StmtPtr & stmt)
{
    if (!stmt)
    {
        return make_error(Errno::InvalidArg, "Null statement");
    }
    cte_plans_.clear();
    cte_recursive_.clear();
    cte_columns_.clear();
    expanding_views_.clear();
    switch (stmt->type)
    {
        case popo::parser::StmtType::Select:
            return plan_select(static_cast<const popo::parser::SelectStatement &>(*stmt));
        case popo::parser::StmtType::Insert:
            return plan_insert(static_cast<const popo::parser::InsertStatement &>(*stmt));
        case popo::parser::StmtType::CreateTable:
            return plan_create_table(static_cast<const popo::parser::CreateTableStatement &>(*stmt));
        case popo::parser::StmtType::CreateIndex:
            return plan_create_index(static_cast<const popo::parser::CreateIndexStatement &>(*stmt));
        case popo::parser::StmtType::Update:
            return plan_update(static_cast<const popo::parser::UpdateStatement &>(*stmt));
        case popo::parser::StmtType::Delete:
            return plan_delete(static_cast<const popo::parser::DeleteStatement &>(*stmt));
        case popo::parser::StmtType::DropTable:
            return plan_drop_table(static_cast<const popo::parser::DropTableStatement &>(*stmt));
        case popo::parser::StmtType::AlterTable:
            return plan_alter_table(static_cast<const popo::parser::AlterTableStatement &>(*stmt));
        case popo::parser::StmtType::ShowTables:
            return plan_show_tables(static_cast<const popo::parser::ShowTablesStatement &>(*stmt));
        case popo::parser::StmtType::Describe:
            return plan_describe(static_cast<const popo::parser::DescribeStatement &>(*stmt));
        case popo::parser::StmtType::ShowDatabases:
            return plan_show_databases(static_cast<const popo::parser::ShowDatabasesStatement &>(*stmt));
        case popo::parser::StmtType::SetVar:
            return plan_set_var(static_cast<const popo::parser::SetVarStatement &>(*stmt));
        case popo::parser::StmtType::ShowVariables:
            return plan_show_variables(static_cast<const popo::parser::ShowVariablesStatement &>(*stmt));
        case popo::parser::StmtType::ShowStatus:
            return plan_show_status(static_cast<const popo::parser::ShowStatusStatement &>(*stmt));
        case popo::parser::StmtType::SelectDatabase:
            return plan_select_database(static_cast<const popo::parser::SelectDatabaseStatement &>(*stmt));
        case popo::parser::StmtType::SelectSystemVar:
            return plan_select_system_var(static_cast<const popo::parser::SelectSystemVarStatement &>(*stmt));
        case popo::parser::StmtType::CreateView:
            return plan_create_view(static_cast<const popo::parser::CreateViewStatement &>(*stmt));
        case popo::parser::StmtType::DropView:
            return plan_drop_view(static_cast<const popo::parser::DropViewStatement &>(*stmt));
        case popo::parser::StmtType::CreateUser:
        case popo::parser::StmtType::DropUser:
        case popo::parser::StmtType::AlterUser:
        case popo::parser::StmtType::Grant:
        case popo::parser::StmtType::Revoke:
        case popo::parser::StmtType::ShowGrants:
            return make_error(Errno::Unknown, "account statement is handled by the session");
        case popo::parser::StmtType::Begin:
        case popo::parser::StmtType::Commit:
        case popo::parser::StmtType::Rollback:
        case popo::parser::StmtType::SetTransaction:
            return make_error(Errno::Unknown, "transaction statement is handled by the session");
    }
    return make_error(Errno::Unknown, "Unknown statement type");
}

Result<PlanNodePtr> Planner::plan_select(const popo::parser::SelectStatement & stmt)
{
    for (const auto & cte : stmt.with_clause)
    {
        cte_recursive_[cte.name] = cte.recursive;
        if (!cte.column_names.empty())
        {
            cte_columns_[cte.name] = cte.column_names;
        }
        if (cte.recursive)
        {
            auto self = std::make_shared<RecursiveScanNode>();
            self->cte_name = cte.name;
            cte_plans_[cte.name] = self;
            auto body = plan_select(*cte.query);
            if (!body)
            {
                return std::unexpected(body.error());
            }
            cte_plans_[cte.name] = *body;
        }
        else
        {
            auto body = plan_select(*cte.query);
            if (!body)
            {
                return std::unexpected(body.error());
            }
            cte_plans_[cte.name] = *body;
        }
    }

    if (stmt.set_ops.empty())
    {
        return plan_select_body(stmt);
    }

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
    auto first_plan = plan_select_body(left_core);
    if (!first_plan)
    {
        return std::unexpected(first_plan.error());
    }
    pieces.push_back(Piece{popo::parser::SetOpKind::Union, false, *first_plan});
    for (const auto & term : stmt.set_ops)
    {
        auto term_plan = plan_select_body(*term.query);
        if (!term_plan)
        {
            return std::unexpected(term_plan.error());
        }
        pieces.push_back(Piece{term.kind, term.is_all, *term_plan});
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

Result<PlanNodePtr> Planner::plan_select_body(const popo::parser::SelectStatement & stmt)
{
    PlanNodePtr plan = nullptr;
    if (stmt.from)
    {
        auto from_plan = plan_table_ref(stmt.from);
        if (!from_plan)
        {
            return std::unexpected(from_plan.error());
        }
        plan = *from_plan;
    }
    else
    {
        plan = std::make_shared<DualNode>();
    }

    if (stmt.where)
    {
        auto filter = std::make_shared<FilterNode>();
        filter->child = plan;
        filter->condition = stmt.where;
        plan = filter;
    }

    bool has_agg = !stmt.group_by.empty() || HasAggregates(stmt.select_list) || (stmt.having && HasAggregate(stmt.having));
    if (has_agg)
    {
        auto eq_ci_gb = [](const std::string & a, const std::string & b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t k = 0; k < a.size(); ++k)
            {
                if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k])))
                    return false;
            }
            return true;
        };
        auto base_cols = plan_output_names(plan);
        auto in_base = [&](const std::string & name)
        {
            for (const auto & c : base_cols)
            {
                if (eq_ci_gb(c, name))
                    return true;
            }
            return false;
        };
        std::vector<popo::parser::ExprPtr> resolved_group_by;
        resolved_group_by.reserve(stmt.group_by.size());
        for (const auto & g : stmt.group_by)
        {
            if (g && g->type == popo::parser::ExprType::Literal)
            {
                const auto & lit = static_cast<const popo::parser::LiteralExpr &>(*g).value;
                if (lit.is_numeric())
                {
                    int64_t ord = static_cast<int64_t>(lit.to_f64());
                    if (ord >= 1 && static_cast<size_t>(ord) <= stmt.select_list.size())
                    {
                        auto target = stmt.select_list[static_cast<size_t>(ord - 1)];
                        if (target && target->type == popo::parser::ExprType::Alias)
                            target = static_cast<const popo::parser::AliasExpr &>(*target).expr;
                        resolved_group_by.push_back(target);
                        continue;
                    }
                }
            }
            if (g && g->type == popo::parser::ExprType::ColumnRef)
            {
                const auto & col = static_cast<const popo::parser::ColumnRefExpr &>(*g);
                if (col.table_name.empty() && !in_base(col.column_name))
                {
                    popo::parser::ExprPtr matched_alias;
                    for (const auto & sel_e : stmt.select_list)
                    {
                        if (sel_e && sel_e->type == popo::parser::ExprType::Alias)
                        {
                            const auto & al = static_cast<const popo::parser::AliasExpr &>(*sel_e);
                            if (eq_ci_gb(al.alias, col.column_name) && !HasAggregate(al.expr))
                            {
                                matched_alias = al.expr;
                                break;
                            }
                        }
                    }
                    if (matched_alias)
                    {
                        resolved_group_by.push_back(matched_alias);
                        continue;
                    }
                }
            }
            resolved_group_by.push_back(g);
        }

        auto agg = std::make_shared<AggNode>();
        agg->child = plan;
        agg->group_by = std::move(resolved_group_by);
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

    if (stmt.having)
    {
        auto filter = std::make_shared<FilterNode>();
        filter->child = plan;
        filter->condition = stmt.having;
        plan = filter;
    }

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

    if (!stmt.order_by.empty())
    {
        auto eq_ci = [](const std::string & a, const std::string & b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t k = 0; k < a.size(); ++k)
            {
                if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k])))
                    return false;
            }
            return true;
        };
        std::vector<std::pair<std::string, popo::parser::ExprPtr>> aliases;
        for (const auto & expr : stmt.select_list)
        {
            if (expr && expr->type == popo::parser::ExprType::Alias)
            {
                const auto & al = static_cast<const popo::parser::AliasExpr &>(*expr);
                aliases.emplace_back(al.alias, al.expr);
            }
        }
        auto rewrite_order = [&](const popo::parser::ExprPtr & expr, auto & self) -> popo::parser::ExprPtr
        {
            if (!expr)
                return expr;
            if (expr->type == popo::parser::ExprType::ColumnRef)
            {
                const auto & col = static_cast<const popo::parser::ColumnRefExpr &>(*expr);
                if (col.table_name.empty())
                {
                    for (const auto & [name, target] : aliases)
                    {
                        if (eq_ci(name, col.column_name))
                            return target;
                    }
                }
                return expr;
            }
            if (expr->type == popo::parser::ExprType::BinaryOp)
            {
                const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
                auto l = self(bin.left, self);
                auto r = self(bin.right, self);
                if (l.get() == bin.left.get() && r.get() == bin.right.get())
                    return expr;
                auto copy = std::make_shared<popo::parser::BinaryOpExpr>();
                copy->op = bin.op;
                copy->left = l;
                copy->right = r;
                return copy;
            }
            if (expr->type == popo::parser::ExprType::UnaryOp)
            {
                const auto & un = static_cast<const popo::parser::UnaryOpExpr &>(*expr);
                auto opnd = self(un.operand, self);
                if (opnd.get() == un.operand.get())
                    return expr;
                auto copy = std::make_shared<popo::parser::UnaryOpExpr>();
                copy->op = un.op;
                copy->operand = opnd;
                return copy;
            }
            return expr;
        };

        std::vector<popo::parser::OrderByItem> resolved_order;
        resolved_order.reserve(stmt.order_by.size());
        for (const auto & item : stmt.order_by)
        {
            popo::parser::OrderByItem copy;
            copy.ascending = item.ascending;
            if (item.expr && item.expr->type == popo::parser::ExprType::Literal)
            {
                const auto & lit = static_cast<const popo::parser::LiteralExpr &>(*item.expr).value;
                if (lit.is_numeric())
                {
                    int64_t ord = static_cast<int64_t>(lit.to_f64());
                    if (ord >= 1 && static_cast<size_t>(ord) <= stmt.select_list.size())
                    {
                        auto target = stmt.select_list[static_cast<size_t>(ord - 1)];
                        if (target && target->type == popo::parser::ExprType::Alias)
                            target = static_cast<const popo::parser::AliasExpr &>(*target).expr;
                        copy.expr = target;
                        resolved_order.push_back(std::move(copy));
                        continue;
                    }
                }
            }
            copy.expr = rewrite_order(item.expr, rewrite_order);
            resolved_order.push_back(std::move(copy));
        }
        if (has_agg)
        {
            PlanNode * walk = plan.get();
            while (walk && walk->type != PlanNodeType::Agg)
            {
                if (walk->type == PlanNodeType::Filter)
                    walk = static_cast<FilterNode *>(walk)->child.get();
                else if (walk->type == PlanNodeType::Window)
                    walk = static_cast<WindowNode *>(walk)->child.get();
                else
                    break;
            }
            if (walk && walk->type == PlanNodeType::Agg)
            {
                auto * agg_ptr = static_cast<AggNode *>(walk);
                for (const auto & item : resolved_order)
                {
                    CollectAggregates(item.expr, agg_ptr->agg_exprs);
                }
                std::unordered_set<const popo::parser::Expr *> seen_aggs;
                std::vector<popo::parser::ExprPtr> unique_aggs;
                unique_aggs.reserve(agg_ptr->agg_exprs.size());
                for (const auto & ae : agg_ptr->agg_exprs)
                {
                    if (seen_aggs.insert(ae.get()).second)
                        unique_aggs.push_back(ae);
                }
                agg_ptr->agg_exprs = std::move(unique_aggs);
            }
        }
        auto sort = std::make_shared<SortNode>();
        sort->child = plan;
        sort->order_by = std::move(resolved_order);
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

    auto project = std::make_shared<ProjectNode>();
    project->child = plan;
    project->expressions = stmt.select_list;
    for (const auto & expr : stmt.select_list)
    {
        project->output_names.push_back(GetExprName(expr));
    }
    plan = project;

    if (stmt.distinct)
    {
        auto distinct = std::make_shared<DistinctNode>();
        distinct->child = plan;
        plan = distinct;
    }

    return plan;
}

Result<PlanNodePtr> Planner::plan_table_ref(const popo::parser::TableRefPtr & ref)
{
    if (!ref)
        return PlanNodePtr{};

    if (ref->type == popo::parser::TableRefType::Simple)
    {
        const auto & simple = static_cast<const popo::parser::SimpleTableRef &>(*ref);
        auto find_cte_ci = [&](const std::string & name) -> decltype(cte_plans_.find(name))
        {
            auto it = cte_plans_.find(name);
            if (it != cte_plans_.end())
                return it;
            for (auto m = cte_plans_.begin(); m != cte_plans_.end(); ++m)
            {
                if (m->first.size() == name.size())
                {
                    bool same = true;
                    for (size_t k = 0; k < name.size(); ++k)
                    {
                        if (std::tolower(static_cast<unsigned char>(m->first[k])) != std::tolower(static_cast<unsigned char>(name[k])))
                        {
                            same = false;
                            break;
                        }
                    }
                    if (same)
                        return m;
                }
            }
            return cte_plans_.end();
        };
        auto cte = find_cte_ci(simple.table_name);
        if (cte != cte_plans_.end())
        {
            if (cte->second && cte->second->type == PlanNodeType::RecursiveScan)
            {
                return cte->second;
            }
            auto scan = std::make_shared<SubqueryScanNode>();
            scan->alias = simple.alias.empty() ? cte->first : simple.alias;
            scan->cte_name = cte->first;
            scan->recursive = cte_recursive_[cte->first];
            if (!simple.column_aliases.empty())
            {
                scan->column_names = simple.column_aliases;
            }
            else if (auto cit = cte_columns_.find(cte->first); cit != cte_columns_.end() && !cit->second.empty())
            {
                scan->column_names = cit->second;
            }
            else
            {
                scan->column_names = plan_output_names(cte->second);
            }
            scan->child = cte->second;
            return scan;
        }
        if (auto view = schema_mgr_.find_view(simple.table_name))
        {
            std::string vkey = simple.table_name;
            for (char & c : vkey)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (expanding_views_.contains(vkey))
            {
                return make_error(Errno::InvalidArg, "view cycle: " + simple.table_name);
            }
            expanding_views_.insert(vkey);
            const auto & body = static_cast<const popo::parser::SelectStatement &>(*view);
            auto child = plan_select(body);
            expanding_views_.erase(vkey);
            if (!child)
            {
                return std::unexpected(child.error());
            }
            auto scan = std::make_shared<SubqueryScanNode>();
            scan->alias = simple.alias.empty() ? simple.table_name : simple.alias;
            scan->child = *child;
            auto vcols = schema_mgr_.find_view_columns(simple.table_name);
            if (!simple.column_aliases.empty())
                scan->column_names = simple.column_aliases;
            else if (!vcols.empty())
                scan->column_names = std::move(vcols);
            else
                scan->column_names = plan_output_names(scan->child);
            return scan;
        }
        auto scan = std::make_shared<ScanNode>();
        scan->table_name = simple.table_name;
        scan->alias = simple.alias;
        POPO_TRY(resolve_scan_columns(*scan));
        if (!simple.column_aliases.empty())
        {
            for (size_t i = 0; i < simple.column_aliases.size() && i < scan->column_names.size(); ++i)
            {
                scan->column_names[i] = simple.column_aliases[i];
            }
        }
        return scan;
    }

    if (ref->type == popo::parser::TableRefType::Derived)
    {
        const auto & derived = static_cast<const popo::parser::DerivedTableRef &>(*ref);
        const auto & query = static_cast<const popo::parser::SelectStatement &>(*derived.query);
        auto child = plan_select(query);
        if (!child)
        {
            return std::unexpected(child.error());
        }
        auto scan = std::make_shared<SubqueryScanNode>();
        scan->alias = derived.alias;
        scan->child = *child;
        scan->column_names = derived.column_aliases.empty() ? plan_output_names(scan->child) : derived.column_aliases;
        return scan;
    }

    if (ref->type == popo::parser::TableRefType::Join)
    {
        const auto & join = static_cast<const popo::parser::JoinTableRef &>(*ref);
        auto left = plan_table_ref(join.left);
        if (!left)
        {
            return std::unexpected(left.error());
        }
        auto right = plan_table_ref(join.right);
        if (!right)
        {
            return std::unexpected(right.error());
        }
        auto node = std::make_shared<JoinNode>();
        node->join_type = join.join_type;
        node->left = *left;
        node->right = *right;
        node->condition = join.condition;
        return node;
    }

    return PlanNodePtr{};
}

Result<void> Planner::resolve_scan_columns(ScanNode & scan)
{
    auto qr = schema_mgr_.query_table(scan.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' not found", scan.table_name));
    }
    auto & schema = *qr;
    scan.table_name = schema->table_name_;
    for (const auto & col : schema->columns_)
    {
        scan.column_names.push_back(col.column_name_);
        scan.column_types.push_back(col.data_type_);
    }
    return {};
}

Result<PlanNodePtr> Planner::plan_insert(const popo::parser::InsertStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<InsertNode>();
    node->table_name = (*qr)->table_name_;
    node->columns = stmt.columns;
    node->values = stmt.values;
    return node;
}

Result<PlanNodePtr> Planner::plan_create_table(const popo::parser::CreateTableStatement & stmt)
{
    auto node = std::make_shared<CreateTableNode>();
    node->table_name = stmt.table_name;
    node->columns = stmt.columns;
    return node;
}

Result<PlanNodePtr> Planner::plan_create_index(const popo::parser::CreateIndexStatement & stmt)
{
    if (stmt.columns.size() != 1)
    {
        return make_error(Errno::InvalidArg, "only a single-column secondary index is supported");
    }
    auto node = std::make_shared<CreateIndexNode>();
    node->unique = stmt.unique;
    node->index_name = stmt.index_name;
    node->table_name = stmt.table_name;
    node->column_name = stmt.columns[0];
    return node;
}

Result<PlanNodePtr> Planner::plan_update(const popo::parser::UpdateStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<UpdateNode>();
    node->table_name = (*qr)->table_name_;
    node->assignments = stmt.assignments;
    node->where_clause = stmt.where;
    return node;
}

Result<PlanNodePtr> Planner::plan_delete(const popo::parser::DeleteStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<DeleteNode>();
    node->table_name = (*qr)->table_name_;
    node->where_clause = stmt.where;
    return node;
}

Result<PlanNodePtr> Planner::plan_drop_table(const popo::parser::DropTableStatement & stmt)
{
    auto node = std::make_shared<DropTableNode>();
    node->table_name = stmt.table_name;
    node->if_exists = stmt.if_exists;
    return node;
}

Result<PlanNodePtr> Planner::plan_alter_table(const popo::parser::AlterTableStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' not found", stmt.table_name));
    }
    auto node = std::make_shared<AlterTableNode>();
    node->table_name = (*qr)->table_name_;
    node->action = stmt.action;
    node->column = stmt.column;
    node->target_name = stmt.target_name;
    node->key_columns = stmt.key_columns;
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
                    static const char * kAggNames[] = {"COUNT", "SUM", "AVG", "MIN", "MAX", "XMLAGG"};
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
        case PlanNodeType::Window:
            return plan_output_names(static_cast<const WindowNode &>(*node).child);
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

Result<PlanNodePtr> Planner::plan_show_tables(const popo::parser::ShowTablesStatement & /*stmt*/)
{
    return std::make_shared<ShowTablesNode>();
}

Result<PlanNodePtr> Planner::plan_describe(const popo::parser::DescribeStatement & stmt)
{
    auto qr = schema_mgr_.query_table(stmt.table_name);
    if (!qr || !*qr)
    {
        return make_error(Errno::TableNotFound, std::format("Table '{}' doesn't exist", stmt.table_name));
    }
    auto node = std::make_shared<DescribeNode>();
    node->table_name = stmt.table_name;
    return node;
}

Result<PlanNodePtr> Planner::plan_show_databases(const popo::parser::ShowDatabasesStatement & /*stmt*/)
{
    return std::make_shared<ShowDatabasesNode>();
}

Result<PlanNodePtr> Planner::plan_set_var(const popo::parser::SetVarStatement & /*stmt*/)
{
    return std::make_shared<SetVarNode>();
}

Result<PlanNodePtr> Planner::plan_show_variables(const popo::parser::ShowVariablesStatement & /*stmt*/)
{
    return std::make_shared<ShowVariablesNode>();
}

Result<PlanNodePtr> Planner::plan_show_status(const popo::parser::ShowStatusStatement & /*stmt*/)
{
    return std::make_shared<ShowStatusNode>();
}

Result<PlanNodePtr> Planner::plan_select_database(const popo::parser::SelectDatabaseStatement & /*stmt*/)
{
    return std::make_shared<SelectDatabaseNode>();
}

Result<PlanNodePtr> Planner::plan_select_system_var(const popo::parser::SelectSystemVarStatement & stmt)
{
    auto node = std::make_shared<SelectSystemVarNode>();
    node->var_names = stmt.var_names;
    node->column_names = stmt.column_names;
    return node;
}

Result<PlanNodePtr> Planner::plan_create_view(const popo::parser::CreateViewStatement & stmt)
{
    if (!stmt.or_replace && schema_mgr_.find_view(stmt.name))
    {
        return make_error(Errno::TableAlreadyExists, "view already exists: " + stmt.name);
    }
    auto existing = schema_mgr_.query_table(stmt.name);
    if (existing && *existing)
    {
        return make_error(Errno::TableAlreadyExists, "name already exists: " + stmt.name);
    }
    auto node = std::make_shared<CreateViewNode>();
    node->name = stmt.name;
    node->or_replace = stmt.or_replace;
    node->column_names = stmt.column_names;
    node->query = stmt.query;
    return node;
}

Result<PlanNodePtr> Planner::plan_drop_view(const popo::parser::DropViewStatement & stmt)
{
    auto node = std::make_shared<DropViewNode>();
    node->name = stmt.name;
    node->if_exists = stmt.if_exists;
    return node;
}

} // namespace popo::planner
