#include "executor/executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "executor/executor_internal.h"
#include "executor/types/col_node.h"
#include "executor/types/equi_edge_info.h"
#include "executor/types/join_pred.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::eq_ci;
using popo::function::lower_str;
using popo::parser::ExprPtr;
using popo::parser::ExprType;

bool Executor::collect_schemas(const popo::planner::PlanNodePtr & node, std::vector<ColumnSchema> & out)
{
    if (!node)
        return false;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::Scan: {
            const auto & scan = static_cast<const popo::planner::ScanNode &>(*node);
            ColumnSchema cs;
            cs.table = scan.alias.empty() ? scan.table_name : scan.alias;
            cs.alias = scan.alias;
            cs.columns = scan.column_names;
            out.push_back(std::move(cs));
            return true;
        }
        case PlanNodeType::SubqueryScan: {
            const auto & sub = static_cast<const popo::planner::SubqueryScanNode &>(*node);
            if (sub.column_names.empty())
                return false;
            ColumnSchema cs;
            cs.table = sub.alias;
            cs.alias = sub.alias;
            cs.columns = sub.column_names;
            out.push_back(std::move(cs));
            return true;
        }
        case PlanNodeType::Filter:
            return collect_schemas(static_cast<const popo::planner::FilterNode &>(*node).child, out);
        case PlanNodeType::Join: {
            const auto & join = static_cast<const popo::planner::JoinNode &>(*node);
            return collect_schemas(join.left, out) && collect_schemas(join.right, out);
        }
        default:
            return false;
    }
}

size_t Executor::estimate_plan_rows(const popo::planner::PlanNodePtr & node) const
{
    if (!node)
        return 1;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::Scan: {
            const auto & s = static_cast<const popo::planner::ScanNode &>(*node);
            size_t pages = 1;
            if (auto * dm = engine_.disk_manager())
                pages = std::max<size_t>(1, dm->page_count(s.table_name));
            return pages * 128;
        }
        case PlanNodeType::Filter: {
            const auto & f = static_cast<const popo::planner::FilterNode &>(*node);
            size_t child_est = estimate_plan_rows(f.child);
            std::vector<ExprPtr> parts;
            split_and_conjuncts(f.condition, parts);
            double factor = 1.0;
            for (const auto & p : parts)
            {
                if (!p)
                    continue;
                if (p->type == ExprType::InSubquery)
                    factor *= 0.05;
                else if (p->type == ExprType::BinaryOp)
                {
                    const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*p);
                    if (b.op == popo::parser::BinaryOpType::Eq)
                        factor *= 0.1;
                    else
                        factor *= 0.3;
                }
                else if (p->type == ExprType::In || p->type == ExprType::Like || p->type == ExprType::Between)
                    factor *= 0.2;
                else
                    factor *= 0.5;
            }
            return std::max<size_t>(1, static_cast<size_t>(static_cast<double>(child_est) * factor));
        }
        case PlanNodeType::Join: {
            const auto & j = static_cast<const popo::planner::JoinNode &>(*node);
            size_t l = estimate_plan_rows(j.left);
            size_t r = estimate_plan_rows(j.right);
            return j.condition ? std::max<size_t>(1, std::min(l, r)) : std::max<size_t>(1, l * r);
        }
        case PlanNodeType::Agg:
            return std::max<size_t>(1, estimate_plan_rows(static_cast<const popo::planner::AggNode &>(*node).child) / 4);
        case PlanNodeType::Limit: {
            const auto & l = static_cast<const popo::planner::LimitNode &>(*node);
            size_t c = estimate_plan_rows(l.child);
            return l.limit_count >= 0 ? std::min<size_t>(c, static_cast<size_t>(l.limit_count)) : c;
        }
        case PlanNodeType::Project:
            return estimate_plan_rows(static_cast<const popo::planner::ProjectNode &>(*node).child);
        case PlanNodeType::Sort:
            return estimate_plan_rows(static_cast<const popo::planner::SortNode &>(*node).child);
        case PlanNodeType::Distinct:
            return estimate_plan_rows(static_cast<const popo::planner::DistinctNode &>(*node).child);
        case PlanNodeType::SubqueryScan:
            return estimate_plan_rows(static_cast<const popo::planner::SubqueryScanNode &>(*node).child);
        default:
            return 100;
    }
}

popo::planner::PlanNodePtr
Executor::optimize_join_group(const popo::planner::PlanNodePtr & root, bool preserve_star_order) const
{
    using popo::planner::FilterNode;
    using popo::planner::JoinNode;
    using popo::planner::PlanNodePtr;
    using popo::planner::PlanNodeType;

    std::vector<PlanNodePtr> leaves;
    std::vector<ExprPtr> conjuncts;

    auto gather = [&](const PlanNodePtr & cur, auto & self) -> void
    {
        if (!cur)
            return;
        if (cur->type == PlanNodeType::Filter)
        {
            const auto & f = static_cast<const FilterNode &>(*cur);
            split_and_conjuncts(f.condition, conjuncts);
            self(f.child, self);
            return;
        }
        if (cur->type == PlanNodeType::Join)
        {
            const auto & j = static_cast<const JoinNode &>(*cur);
            if (j.join_type == popo::parser::JoinType::Inner || j.join_type == popo::parser::JoinType::Cross)
            {
                if (j.condition)
                    split_and_conjuncts(j.condition, conjuncts);
                self(j.left, self);
                self(j.right, self);
                return;
            }
        }
        leaves.push_back(optimize_plan(cur, false));
    };
    gather(root, gather);

    if (leaves.size() <= 1)
    {
        if (leaves.empty())
            return root;
        for (auto & c : conjuncts)
            c = fold_constants(c);
        PlanNodePtr out = leaves[0];
        if (!conjuncts.empty())
        {
            auto f = std::make_shared<FilterNode>();
            f->child = out;
            f->condition = combine_and(conjuncts);
            out = f;
        }
        return out;
    }

    for (auto & c : conjuncts)
        c = fold_constants(c);

    std::vector<ExprPtr> extra_equijoins;
    for (const auto & c : conjuncts)
        extract_common_or_equijoins(c, extra_equijoins);
    conjuncts.insert(conjuncts.end(), extra_equijoins.begin(), extra_equijoins.end());

    const size_t m = leaves.size();
    std::vector<std::vector<ColumnSchema>> leaf_schemas(m);
    for (size_t i = 0; i < m; ++i)
    {
        if (!collect_schemas(leaves[i], leaf_schemas[i]))
        {
            if (root->type == PlanNodeType::Filter)
            {
                const auto & f = static_cast<const FilterNode &>(*root);
                auto copy = std::make_shared<FilterNode>();
                copy->child = optimize_plan(f.child, preserve_star_order);
                copy->condition = f.condition;
                return copy;
            }
            const auto & j = static_cast<const JoinNode &>(*root);
            auto copy = std::make_shared<JoinNode>();
            copy->join_type = j.join_type;
            copy->left = optimize_plan(j.left, preserve_star_order);
            copy->right = optimize_plan(j.right, preserve_star_order);
            copy->condition = j.condition;
            return copy;
        }
    }

    auto schema_has_col = [](const ColumnSchema & s, const std::string & table, const std::string & col) -> bool
    {
        bool tmatch = table.empty() || eq_ci(table, s.table) || (!s.alias.empty() && eq_ci(table, s.alias));
        if (!tmatch)
            return false;
        for (const auto & c : s.columns)
        {
            if (eq_ci(c, col))
                return true;
        }
        return false;
    };

    auto expr_leaf_deps = [&](const ExprPtr & expr, std::set<size_t> & deps, bool & ambiguous)
    {
        deps.clear();
        ambiguous = false;
        std::vector<const popo::parser::ColumnRefExpr *> refs;
        collect_col_refs(expr, refs);
        for (const auto * ref : refs)
        {
            size_t hits = 0;
            size_t hit_leaf = 0;
            for (size_t i = 0; i < m; ++i)
            {
                bool in_leaf = false;
                for (const auto & s : leaf_schemas[i])
                {
                    if (schema_has_col(s, ref->table_name, ref->column_name))
                    {
                        in_leaf = true;
                        break;
                    }
                }
                if (in_leaf)
                {
                    ++hits;
                    hit_leaf = i;
                }
            }
            if (hits > 1)
            {
                ambiguous = true;
                return;
            }
            if (hits == 1)
                deps.insert(hit_leaf);
        }
    };

    std::vector<std::vector<ExprPtr>> leaf_filters(m);
    std::vector<ExprPtr> const_filters;
    std::vector<JoinPred> join_preds;
    std::vector<ExprPtr> post_filters;

    auto is_stmt_uncorrelated = [&](const popo::parser::StmtPtr & stmt) -> bool
    {
        if (!stmt || stmt->type != popo::parser::StmtType::Select)
            return false;
        const auto & sel = static_cast<const popo::parser::SelectStatement &>(*stmt);
        std::vector<std::string> inner_tables;
        collect_table_names_from_ref(sel.from, inner_tables);
        std::vector<ColumnSchema> inner_schemas;
        for (const auto & tn : inner_tables)
        {
            auto qr = schema_mgr_.query_table(tn);
            if (qr && *qr)
            {
                ColumnSchema cs;
                cs.table = tn;
                for (const auto & col : (*qr)->columns_)
                    cs.columns.push_back(col.column_name_);
                inner_schemas.push_back(std::move(cs));
            }
        }
        auto add_ref_schemas = [&](const popo::parser::TableRefPtr & ref, auto & self) -> void
        {
            if (!ref)
                return;
            if (ref->type == popo::parser::TableRefType::Simple)
            {
                const auto & s = static_cast<const popo::parser::SimpleTableRef &>(*ref);
                auto qr = schema_mgr_.query_table(s.table_name);
                if (qr && *qr)
                {
                    ColumnSchema cs;
                    cs.table = s.alias.empty() ? s.table_name : s.alias;
                    cs.alias = s.alias;
                    for (const auto & col : (*qr)->columns_)
                        cs.columns.push_back(col.column_name_);
                    inner_schemas.push_back(std::move(cs));
                }
            }
            else if (ref->type == popo::parser::TableRefType::Join)
            {
                const auto & j = static_cast<const popo::parser::JoinTableRef &>(*ref);
                self(j.left, self);
                self(j.right, self);
            }
        };
        add_ref_schemas(sel.from, add_ref_schemas);
        if (inner_schemas.empty())
            return false;

        std::vector<const popo::parser::ColumnRefExpr *> sub_refs;
        bool sub_star = false;
        collect_select_col_refs(sel, sub_refs, sub_star);
        for (const auto * r : sub_refs)
        {
            bool in_inner = false;
            for (const auto & s : inner_schemas)
            {
                if (schema_has_col(s, r->table_name, r->column_name))
                {
                    in_inner = true;
                    break;
                }
            }
            if (!in_inner)
            {
                for (size_t li = 0; li < m; ++li)
                {
                    for (const auto & s : leaf_schemas[li])
                    {
                        if (schema_has_col(s, r->table_name, r->column_name))
                            return false;
                    }
                }
            }
        }
        return true;
    };

    for (const auto & c : conjuncts)
    {
        if (expr_has_subquery_or_agg(c))
        {
            if (c && c->type == ExprType::InSubquery)
            {
                const auto & in_sub = static_cast<const popo::parser::InSubqueryExpr &>(*c);
                if (!expr_has_subquery_or_agg(in_sub.operand) && is_stmt_uncorrelated(in_sub.query))
                {
                    std::set<size_t> deps;
                    bool ambiguous = false;
                    expr_leaf_deps(in_sub.operand, deps, ambiguous);
                    if (!ambiguous && deps.size() == 1)
                    {
                        leaf_filters[*deps.begin()].push_back(c);
                        continue;
                    }
                }
            }
            post_filters.push_back(c);
            continue;
        }
        std::set<size_t> deps;
        bool ambiguous = false;
        expr_leaf_deps(c, deps, ambiguous);
        if (ambiguous)
        {
            post_filters.push_back(c);
            continue;
        }
        if (deps.empty())
        {
            const_filters.push_back(c);
        }
        else if (deps.size() == 1)
        {
            leaf_filters[*deps.begin()].push_back(c);
        }
        else
        {
            if (c->type == ExprType::BinaryOp
                && static_cast<const popo::parser::BinaryOpExpr &>(*c).op == popo::parser::BinaryOpType::Or)
            {
                std::vector<ExprPtr> branches;
                split_or_branches(c, branches);
                if (branches.size() >= 2)
                {
                    std::vector<std::vector<ExprPtr>> b_conj(branches.size());
                    for (size_t bi = 0; bi < branches.size(); ++bi)
                        split_and_conjuncts(branches[bi], b_conj[bi]);
                    for (size_t li = 0; li < m; ++li)
                    {
                        std::vector<ExprPtr> per_branch;
                        bool all_branches = true;
                        for (size_t bi = 0; bi < branches.size(); ++bi)
                        {
                            std::vector<ExprPtr> leaf_parts;
                            for (const auto & bc : b_conj[bi])
                            {
                                if (expr_has_subquery_or_agg(bc))
                                    continue;
                                std::set<size_t> bdeps;
                                bool bamb = false;
                                expr_leaf_deps(bc, bdeps, bamb);
                                if (!bamb && bdeps.size() == 1 && *bdeps.begin() == li)
                                    leaf_parts.push_back(bc);
                            }
                            if (leaf_parts.empty())
                            {
                                all_branches = false;
                                break;
                            }
                            per_branch.push_back(combine_and(leaf_parts));
                        }
                        if (all_branches && !per_branch.empty())
                            leaf_filters[li].push_back(combine_or(per_branch));
                    }
                }
            }
            bool is_equi = false;
            if (c->type == ExprType::BinaryOp)
            {
                const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*c);
                if (b.op == popo::parser::BinaryOpType::Eq && deps.size() == 2)
                {
                    std::set<size_t> ld, rd;
                    bool la = false, ra = false;
                    expr_leaf_deps(b.left, ld, la);
                    expr_leaf_deps(b.right, rd, ra);
                    if (!la && !ra && ld.size() == 1 && rd.size() == 1 && *ld.begin() != *rd.begin())
                        is_equi = true;
                }
            }
            join_preds.push_back(JoinPred{c, std::move(deps), is_equi, false});
        }
    }

    std::vector<bool> has_pushed_filter(m, false);
    for (size_t i = 0; i < m; ++i)
    {
        if (i == 0 && !const_filters.empty())
            leaf_filters[0].insert(leaf_filters[0].begin(), const_filters.begin(), const_filters.end());
        if (!leaf_filters[i].empty())
        {
            has_pushed_filter[i] = true;
            auto f = std::make_shared<FilterNode>();
            f->child = leaves[i];
            f->condition = combine_and(leaf_filters[i]);
            leaves[i] = f;
        }
    }

    std::vector<size_t> leaf_est(m, 100);
    std::vector<size_t> leaf_base(m, 100);
    std::vector<std::string> leaf_pk(m);
    for (size_t i = 0; i < m; ++i)
    {
        leaf_est[i] = estimate_plan_rows(leaves[i]);
        auto base_node = leaves[i];
        while (base_node && base_node->type == PlanNodeType::Filter)
            base_node = static_cast<const FilterNode &>(*base_node).child;
        if (base_node && base_node->type == PlanNodeType::Scan)
        {
            leaf_base[i] = estimate_plan_rows(base_node);
            const auto & sc = static_cast<const popo::planner::ScanNode &>(*base_node);
            auto qr = schema_mgr_.query_table(sc.table_name);
            if (qr && *qr && (*qr)->primary_key_col_idx_ >= 0
                && static_cast<size_t>((*qr)->primary_key_col_idx_) < (*qr)->columns_.size())
            {
                leaf_pk[i] = lower_str((*qr)->columns_[static_cast<size_t>((*qr)->primary_key_col_idx_)].column_name_);
            }
        }
        else
        {
            leaf_base[i] = leaf_est[i];
        }
    }

    std::vector<ColNode> col_nodes;
    std::unordered_map<std::string, size_t> col_node_id;
    auto get_col_node = [&](size_t leaf, const std::string & col, const ExprPtr & expr) -> size_t
    {
        std::string key = std::to_string(leaf) + ":" + lower_str(col);
        auto it = col_node_id.find(key);
        if (it != col_node_id.end())
            return it->second;
        size_t id = col_nodes.size();
        col_nodes.push_back(ColNode{leaf, lower_str(col), expr});
        col_node_id.emplace(std::move(key), id);
        return id;
    };
    std::vector<size_t> uf_parent;
    auto uf_find = [&](size_t x) -> size_t
    {
        while (uf_parent[x] != x)
        {
            uf_parent[x] = uf_parent[uf_parent[x]];
            x = uf_parent[x];
        }
        return x;
    };
    auto uf_union = [&](size_t a, size_t b)
    {
        a = uf_find(a);
        b = uf_find(b);
        if (a != b)
            uf_parent[a] = b;
    };

    std::set<std::pair<size_t, size_t>> explicit_equi_pairs;
    for (const auto & jp : join_preds)
    {
        if (!jp.is_equi || !jp.expr || jp.expr->type != ExprType::BinaryOp)
            continue;
        const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*jp.expr);
        if (b.left->type != ExprType::ColumnRef || b.right->type != ExprType::ColumnRef)
            continue;
        std::set<size_t> ld, rd;
        bool la = false, ra = false;
        expr_leaf_deps(b.left, ld, la);
        expr_leaf_deps(b.right, rd, ra);
        if (la || ra || ld.size() != 1 || rd.size() != 1 || *ld.begin() == *rd.begin())
            continue;
        const auto & lc = static_cast<const popo::parser::ColumnRefExpr &>(*b.left);
        const auto & rc = static_cast<const popo::parser::ColumnRefExpr &>(*b.right);
        size_t id1 = get_col_node(*ld.begin(), lc.column_name, b.left);
        size_t id2 = get_col_node(*rd.begin(), rc.column_name, b.right);
        while (uf_parent.size() < col_nodes.size())
            uf_parent.push_back(uf_parent.size());
        uf_union(id1, id2);
        explicit_equi_pairs.emplace(std::min(id1, id2), std::max(id1, id2));
    }

    std::unordered_map<size_t, std::vector<size_t>> eq_classes;
    for (size_t id = 0; id < col_nodes.size(); ++id)
        eq_classes[uf_find(id)].push_back(id);

    std::unordered_map<size_t, size_t> class_ndv;
    for (const auto & [root_id, members] : eq_classes)
    {
        size_t min_base = std::numeric_limits<size_t>::max();
        for (size_t id : members)
            min_base = std::min(min_base, std::max<size_t>(leaf_base[col_nodes[id].leaf], 1));
        class_ndv[root_id] = min_base;
        if (members.size() >= 3)
        {
            for (size_t a = 0; a < members.size(); ++a)
            {
                for (size_t b = a + 1; b < members.size(); ++b)
                {
                    size_t id1 = members[a];
                    size_t id2 = members[b];
                    if (col_nodes[id1].leaf == col_nodes[id2].leaf)
                        continue;
                    auto pk = std::make_pair(std::min(id1, id2), std::max(id1, id2));
                    if (explicit_equi_pairs.contains(pk))
                        continue;
                    explicit_equi_pairs.insert(pk);
                    auto eq = std::make_shared<popo::parser::BinaryOpExpr>();
                    eq->op = popo::parser::BinaryOpType::Eq;
                    eq->left = col_nodes[id1].expr;
                    eq->right = col_nodes[id2].expr;
                    std::set<size_t> deps{col_nodes[id1].leaf, col_nodes[id2].leaf};
                    join_preds.push_back(JoinPred{eq, std::move(deps), true, false});
                }
            }
        }
    }

    std::vector<EquiEdgeInfo> equi_edges;
    for (const auto & jp : join_preds)
    {
        if (!jp.is_equi || !jp.expr || jp.expr->type != ExprType::BinaryOp)
            continue;
        const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*jp.expr);
        if (b.left->type == ExprType::ColumnRef && b.right->type == ExprType::ColumnRef)
        {
            std::set<size_t> ld, rd;
            bool la = false, ra = false;
            expr_leaf_deps(b.left, ld, la);
            expr_leaf_deps(b.right, rd, ra);
            if (!la && !ra && ld.size() == 1 && rd.size() == 1 && *ld.begin() != *rd.begin())
            {
                size_t l1 = *ld.begin();
                size_t l2 = *rd.begin();
                const auto & lc = static_cast<const popo::parser::ColumnRefExpr &>(*b.left);
                const auto & rc = static_cast<const popo::parser::ColumnRefExpr &>(*b.right);
                std::string k1 = std::to_string(l1) + ":" + lower_str(lc.column_name);
                auto it = col_node_id.find(k1);
                size_t root_id = (it != col_node_id.end()) ? uf_find(it->second) : 0;
                double ndv = (it != col_node_id.end() && class_ndv.contains(root_id))
                    ? static_cast<double>(class_ndv[root_id])
                    : static_cast<double>(std::max<size_t>(std::min(leaf_base[l1], leaf_base[l2]), 1));
                equi_edges.push_back(EquiEdgeInfo{
                    l1, l2, root_id, ndv,
                    !leaf_pk[l1].empty() && lower_str(lc.column_name) == leaf_pk[l1],
                    !leaf_pk[l2].empty() && lower_str(rc.column_name) == leaf_pk[l2]});
            }
        }
    }

    std::vector<size_t> order;
    order.reserve(m);
    if (preserve_star_order)
    {
        for (size_t i = 0; i < m; ++i)
            order.push_back(i);
    }
    else if (m <= 10)
    {
        const uint32_t full = (1u << m) - 1u;
        std::vector<double> dp_cost(1u << m, std::numeric_limits<double>::infinity());
        std::vector<double> dp_rows(1u << m, 1.0);
        std::vector<uint8_t> dp_last(1u << m, 0);

        for (size_t i = 0; i < m; ++i)
        {
            uint32_t s = 1u << i;
            dp_rows[s] = static_cast<double>(std::max<size_t>(leaf_est[i], 1));
            dp_cost[s] = dp_rows[s];
            dp_last[s] = static_cast<uint8_t>(i);
        }

        for (uint32_t mask = 1; mask <= full; ++mask)
        {
            if (!std::isfinite(dp_cost[mask]))
                continue;
            const double cur_rows = dp_rows[mask];
            for (size_t cand = 0; cand < m; ++cand)
            {
                if (mask & (1u << cand))
                    continue;
                const double c_est = static_cast<double>(std::max<size_t>(leaf_est[cand], 1));
                const double c_base = static_cast<double>(std::max<size_t>(leaf_base[cand], 1));

                double selectivity_div = 1.0;
                bool has_equi = false;
                bool cand_pk_matched = false;
                std::set<size_t> used_classes;
                for (const auto & e : equi_edges)
                {
                    bool hit = false;
                    bool c_pk = false;
                    if (e.leaf_b == cand && (mask & (1u << e.leaf_a)))
                    {
                        hit = true;
                        c_pk = e.b_is_pk;
                    }
                    else if (e.leaf_a == cand && (mask & (1u << e.leaf_b)))
                    {
                        hit = true;
                        c_pk = e.a_is_pk;
                    }
                    if (!hit)
                        continue;
                    has_equi = true;
                    if (c_pk)
                        cand_pk_matched = true;
                    if (used_classes.insert(e.eq_root).second)
                        selectivity_div *= std::max(e.ndv, 1.0);
                }

                bool has_any = has_equi;
                if (!has_equi)
                {
                    for (const auto & jp : join_preds)
                    {
                        if (!jp.deps.contains(cand))
                            continue;
                        bool ready = true;
                        for (size_t d : jp.deps)
                        {
                            if (d != cand && !(mask & (1u << d)))
                            {
                                ready = false;
                                break;
                            }
                        }
                        if (ready)
                        {
                            has_any = true;
                            if (jp.is_equi)
                            {
                                has_equi = true;
                                selectivity_div = std::max(c_base, 10.0);
                            }
                        }
                    }
                }

                double next_rows = 1.0;
                double penalty = 0.0;
                if (has_equi)
                {
                    next_rows = (cur_rows * c_est) / selectivity_div;
                    if (cand_pk_matched)
                        next_rows = std::min(next_rows, cur_rows * (c_est / c_base));
                    next_rows = std::max(next_rows, 1.0);
                }
                else if (has_any)
                {
                    next_rows = std::max(cur_rows * c_est * 0.5, 1.0);
                    penalty = cur_rows * c_est;
                }
                else
                {
                    next_rows = std::max(cur_rows * c_est, 1.0);
                    penalty = 1e12 + cur_rows * c_est;
                }

                const double build_rows = std::min(cur_rows, c_est);
                const double probe_rows = std::max(cur_rows, c_est);
                const double step_cost = build_rows * 3.0 + probe_rows + next_rows * 1.5 + penalty;
                const uint32_t next_mask = mask | (1u << cand);
                const double cand_cost = dp_cost[mask] + step_cost;
                if (cand_cost < dp_cost[next_mask])
                {
                    dp_cost[next_mask] = cand_cost;
                    dp_rows[next_mask] = next_rows;
                    dp_last[next_mask] = static_cast<uint8_t>(cand);
                }
            }
        }

        order.resize(m);
        uint32_t cur = full;
        for (size_t idx = m; idx > 0; --idx)
        {
            uint8_t last = dp_last[cur];
            order[idx - 1] = last;
            cur &= ~(1u << last);
        }
    }
    else
    {
        std::vector<bool> used(m, false);
        size_t first = 0;
        for (size_t i = 1; i < m; ++i)
        {
            if (leaf_est[i] < leaf_est[first]
                || (leaf_est[i] == leaf_est[first] && has_pushed_filter[i] && !has_pushed_filter[first]))
            {
                first = i;
            }
        }
        used[first] = true;
        order.push_back(first);

        auto in_joined = [&](size_t idx) { return used[idx]; };
        while (order.size() < m)
        {
            size_t best = m;
            int best_tier = -1;
            size_t best_est = std::numeric_limits<size_t>::max();
            for (size_t cand = 0; cand < m; ++cand)
            {
                if (used[cand])
                    continue;
                bool has_equi = false;
                bool has_any = false;
                for (const auto & jp : join_preds)
                {
                    if (!jp.deps.contains(cand))
                        continue;
                    bool all_ready = true;
                    for (size_t d : jp.deps)
                    {
                        if (d != cand && !in_joined(d))
                        {
                            all_ready = false;
                            break;
                        }
                    }
                    if (all_ready)
                    {
                        has_any = true;
                        if (jp.is_equi)
                            has_equi = true;
                    }
                }
                int tier = has_equi ? 2 : (has_any ? 1 : 0);
                if (tier > best_tier || (tier == best_tier && leaf_est[cand] < best_est))
                {
                    best_tier = tier;
                    best_est = leaf_est[cand];
                    best = cand;
                }
            }
            if (best == m)
            {
                for (size_t cand = 0; cand < m; ++cand)
                {
                    if (!used[cand])
                    {
                        best = cand;
                        break;
                    }
                }
            }
            used[best] = true;
            order.push_back(best);
        }
    }

    std::vector<bool> joined(m, false);
    joined[order[0]] = true;
    PlanNodePtr current = leaves[order[0]];

    for (size_t step = 1; step < m; ++step)
    {
        size_t next = order[step];
        joined[next] = true;
        std::vector<ExprPtr> ready;
        for (auto & jp : join_preds)
        {
            if (jp.placed)
                continue;
            bool all_in = true;
            for (size_t d : jp.deps)
            {
                if (!joined[d])
                {
                    all_in = false;
                    break;
                }
            }
            if (all_in)
            {
                ready.push_back(jp.expr);
                jp.placed = true;
            }
        }
        auto jnode = std::make_shared<JoinNode>();
        jnode->left = current;
        jnode->right = leaves[next];
        jnode->condition = combine_and(ready);
        jnode->join_type = jnode->condition ? popo::parser::JoinType::Inner : popo::parser::JoinType::Cross;
        current = jnode;
    }

    for (const auto & jp : join_preds)
    {
        if (!jp.placed)
            post_filters.push_back(jp.expr);
    }
    if (!post_filters.empty())
    {
        auto f = std::make_shared<FilterNode>();
        f->child = current;
        f->condition = combine_and(post_filters);
        current = f;
    }
    return current;
}

popo::planner::PlanNodePtr
Executor::optimize_plan(const popo::planner::PlanNodePtr & node, bool preserve_star_order) const
{
    if (!node)
        return nullptr;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::Project: {
            const auto & p = static_cast<const popo::planner::ProjectNode &>(*node);
            bool has_unqualified_star = false;
            auto copy = std::make_shared<popo::planner::ProjectNode>(p);
            for (auto & e : copy->expressions)
            {
                e = fold_constants(e);
                if (e && e->type == ExprType::Star && static_cast<const popo::parser::StarExpr &>(*e).table_name.empty())
                    has_unqualified_star = true;
            }
            copy->child = optimize_plan(p.child, has_unqualified_star);
            return copy;
        }
        case PlanNodeType::Agg: {
            const auto & a = static_cast<const popo::planner::AggNode &>(*node);
            auto copy = std::make_shared<popo::planner::AggNode>(a);
            for (auto & g : copy->group_by)
                g = fold_constants(g);
            for (auto & ae : copy->agg_exprs)
                ae = fold_constants(ae);
            copy->child = optimize_plan(a.child, false);
            return copy;
        }
        case PlanNodeType::Sort: {
            const auto & s = static_cast<const popo::planner::SortNode &>(*node);
            auto copy = std::make_shared<popo::planner::SortNode>(s);
            for (auto & o : copy->order_by)
                o.expr = fold_constants(o.expr);
            copy->child = optimize_plan(s.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Limit: {
            const auto & l = static_cast<const popo::planner::LimitNode &>(*node);
            auto copy = std::make_shared<popo::planner::LimitNode>(l);
            copy->child = optimize_plan(l.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Distinct: {
            const auto & d = static_cast<const popo::planner::DistinctNode &>(*node);
            auto copy = std::make_shared<popo::planner::DistinctNode>(d);
            copy->child = optimize_plan(d.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Window: {
            const auto & w = static_cast<const popo::planner::WindowNode &>(*node);
            auto copy = std::make_shared<popo::planner::WindowNode>(w);
            copy->child = optimize_plan(w.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::SetOp: {
            const auto & op = static_cast<const popo::planner::SetOpNode &>(*node);
            auto copy = std::make_shared<popo::planner::SetOpNode>(op);
            copy->left = optimize_plan(op.left, false);
            copy->right = optimize_plan(op.right, false);
            return copy;
        }
        case PlanNodeType::SubqueryScan: {
            const auto & sub = static_cast<const popo::planner::SubqueryScanNode &>(*node);
            if (sub.recursive || !sub.cte_name.empty())
                return node;
            auto copy = std::make_shared<popo::planner::SubqueryScanNode>(sub);
            copy->child = optimize_plan(sub.child, false);
            return copy;
        }
        case PlanNodeType::Filter: {
            const auto & f = static_cast<const popo::planner::FilterNode &>(*node);
            if (f.child && (f.child->type == PlanNodeType::Agg || f.child->type == PlanNodeType::Window))
            {
                auto copy = std::make_shared<popo::planner::FilterNode>(f);
                copy->condition = fold_constants(f.condition);
                copy->child = optimize_plan(f.child, preserve_star_order);
                return copy;
            }
            return optimize_join_group(node, preserve_star_order);
        }
        case PlanNodeType::Join: {
            const auto & j = static_cast<const popo::planner::JoinNode &>(*node);
            if (j.join_type == popo::parser::JoinType::Inner || j.join_type == popo::parser::JoinType::Cross)
                return optimize_join_group(node, preserve_star_order);
            auto copy = std::make_shared<popo::planner::JoinNode>(j);
            copy->condition = fold_constants(j.condition);
            copy->left = optimize_plan(j.left, preserve_star_order);
            copy->right = optimize_plan(j.right, preserve_star_order);
            if (copy->join_type == popo::parser::JoinType::Left && copy->condition)
            {
                std::vector<ColumnSchema> lschemas, rschemas;
                if (collect_schemas(copy->left, lschemas) && collect_schemas(copy->right, rschemas))
                {
                    auto in_s = [](const std::vector<ColumnSchema> & ss, const std::string & t, const std::string & c)
                    {
                        for (const auto & s : ss)
                        {
                            bool tm = t.empty() || eq_ci(t, s.table) || (!s.alias.empty() && eq_ci(t, s.alias));
                            if (!tm) continue;
                            for (const auto & col : s.columns)
                                if (eq_ci(col, c)) return true;
                        }
                        return false;
                    };
                    std::vector<ExprPtr> parts, push_right, keep;
                    split_and_conjuncts(copy->condition, parts);
                    for (const auto & part : parts)
                    {
                        if (expr_has_subquery_or_agg(part))
                        {
                            keep.push_back(part);
                            continue;
                        }
                        std::vector<const popo::parser::ColumnRefExpr *> refs;
                        collect_col_refs(part, refs);
                        bool has_l = false, has_r = false;
                        for (const auto * r : refs)
                        {
                            if (in_s(lschemas, r->table_name, r->column_name)) has_l = true;
                            if (in_s(rschemas, r->table_name, r->column_name)) has_r = true;
                        }
                        if (has_r && !has_l)
                            push_right.push_back(part);
                        else
                            keep.push_back(part);
                    }
                    if (!push_right.empty())
                    {
                        auto rf = std::make_shared<popo::planner::FilterNode>();
                        rf->child = copy->right;
                        rf->condition = combine_and(push_right);
                        copy->right = rf;
                        copy->condition = combine_and(keep);
                    }
                }
            }
            return copy;
        }
        default:
            return node;
    }
}

popo::planner::PlanNodePtr Executor::push_projections(const popo::planner::PlanNodePtr & root) const
{
    if (!root)
        return nullptr;
    using popo::planner::PlanNodePtr;
    using popo::planner::PlanNodeType;

    auto walk = [&](const PlanNodePtr & node, std::unordered_set<std::string> needed, bool keep_all, auto & self) -> PlanNodePtr
    {
        if (!node)
            return nullptr;
        auto add_expr = [&](const ExprPtr & expr)
        {
            std::vector<const popo::parser::ColumnRefExpr *> refs;
            bool star = false;
            collect_col_refs_deep(expr, refs, star);
            if (star)
                keep_all = true;
            for (const auto * r : refs)
                needed.insert(lower_str(r->column_name));
        };
        switch (node->type)
        {
            case PlanNodeType::Scan: {
                if (keep_all)
                    return node;
                const auto & s = static_cast<const popo::planner::ScanNode &>(*node);
                auto qr = schema_mgr_.query_table(s.table_name);
                if (!qr || !*qr || (*qr)->columns_.empty())
                    return node;
                const auto & cols = (*qr)->columns_;
                std::vector<uint16_t> proj;
                for (uint16_t i = 0; i < cols.size(); ++i)
                {
                    const std::string & cname = (!s.column_names.empty() && i < s.column_names.size())
                        ? s.column_names[i] : cols[i].column_name_;
                    if (needed.contains(lower_str(cname)))
                        proj.push_back(i);
                }
                if (proj.empty())
                    proj.push_back(0);
                if (proj.size() >= cols.size())
                    return node;
                auto copy = std::make_shared<popo::planner::ScanNode>(s);
                copy->projected_columns = std::move(proj);
                copy->has_projection = true;
                return copy;
            }
            case PlanNodeType::Project: {
                const auto & p = static_cast<const popo::planner::ProjectNode &>(*node);
                std::unordered_set<std::string> child_needed;
                bool child_keep_all = false;
                for (const auto & e : p.expressions)
                {
                    std::vector<const popo::parser::ColumnRefExpr *> refs;
                    bool star = false;
                    collect_col_refs_deep(e, refs, star);
                    if (star)
                        child_keep_all = true;
                    for (const auto * r : refs)
                        child_needed.insert(lower_str(r->column_name));
                }
                auto copy = std::make_shared<popo::planner::ProjectNode>(p);
                copy->child = self(p.child, std::move(child_needed), child_keep_all, self);
                return copy;
            }
            case PlanNodeType::Filter: {
                const auto & f = static_cast<const popo::planner::FilterNode &>(*node);
                add_expr(f.condition);
                auto copy = std::make_shared<popo::planner::FilterNode>(f);
                copy->child = self(f.child, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::Join: {
                const auto & j = static_cast<const popo::planner::JoinNode &>(*node);
                add_expr(j.condition);
                auto copy = std::make_shared<popo::planner::JoinNode>(j);
                copy->left = self(j.left, needed, keep_all, self);
                copy->right = self(j.right, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::Agg: {
                const auto & a = static_cast<const popo::planner::AggNode &>(*node);
                for (const auto & g : a.group_by)
                    add_expr(g);
                for (const auto & ae : a.agg_exprs)
                    add_expr(ae);
                auto copy = std::make_shared<popo::planner::AggNode>(a);
                copy->child = self(a.child, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::Sort: {
                const auto & s = static_cast<const popo::planner::SortNode &>(*node);
                for (const auto & o : s.order_by)
                    add_expr(o.expr);
                auto copy = std::make_shared<popo::planner::SortNode>(s);
                copy->child = self(s.child, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::Limit: {
                const auto & l = static_cast<const popo::planner::LimitNode &>(*node);
                auto copy = std::make_shared<popo::planner::LimitNode>(l);
                copy->child = self(l.child, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::Distinct: {
                const auto & d = static_cast<const popo::planner::DistinctNode &>(*node);
                auto copy = std::make_shared<popo::planner::DistinctNode>(d);
                copy->child = self(d.child, std::move(needed), true, self);
                return copy;
            }
            case PlanNodeType::Window: {
                const auto & w = static_cast<const popo::planner::WindowNode &>(*node);
                for (const auto & we : w.windows)
                    add_expr(we);
                auto copy = std::make_shared<popo::planner::WindowNode>(w);
                copy->child = self(w.child, std::move(needed), keep_all, self);
                return copy;
            }
            case PlanNodeType::SubqueryScan: {
                const auto & sub = static_cast<const popo::planner::SubqueryScanNode &>(*node);
                if (sub.recursive || !sub.cte_name.empty())
                    return node;
                auto copy = std::make_shared<popo::planner::SubqueryScanNode>(sub);
                copy->child = self(sub.child, {}, false, self);
                return copy;
            }
            case PlanNodeType::SetOp: {
                const auto & op = static_cast<const popo::planner::SetOpNode &>(*node);
                auto copy = std::make_shared<popo::planner::SetOpNode>(op);
                copy->left = self(op.left, {}, false, self);
                copy->right = self(op.right, {}, false, self);
                return copy;
            }
            default:
                return node;
        }
    };
    return walk(root, {}, false, walk);
}

} // namespace popo::executor
