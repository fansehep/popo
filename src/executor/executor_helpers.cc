#include "executor/executor_internal.h"

#include <cstdlib>
#include <cstring>

#include "executor/vector/vector_pred.h"
#include "function/date_functions.h"
#include "function/value_functions.h"

namespace popo::executor::detail
{

using popo::function::add_date_interval;
using popo::function::do_arithmetic;
using popo::function::eq_ci;
using popo::function::is_floating;
using popo::function::is_null;
using popo::function::to_i64;
using popo::parser::BinaryOpType;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Value;
using popo::type::ValueType;

std::unexpected<popo::base::Error> executor_error(std::string message, popo::base::Errno code)
{
    return popo::base::make_error(code, std::move(message));
}

ExecutorOptions default_executor_options()
{
    ExecutorOptions options;
    if (const char * raw = std::getenv("POPO_EXECUTOR_MEMORY_BYTES"))
    {
        auto parsed = popo::base::parse_i64(raw);
        if (parsed && *parsed >= 4096)
            options.memory_limit_bytes = static_cast<size_t>(*parsed);
    }
    if (const char * directory = std::getenv("POPO_EXECUTOR_TEMP_DIR"))
        options.temp_directory = directory;
    return options;
}

void split_and_conjuncts(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == BinaryOpType::And)
        {
            split_and_conjuncts(bin.left, out);
            split_and_conjuncts(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

void split_or_branches(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == BinaryOpType::Or)
        {
            split_or_branches(bin.left, out);
            split_or_branches(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

bool same_col_ref(const ExprPtr & a, const ExprPtr & b)
{
    if (!a || !b || a->type != ExprType::ColumnRef || b->type != ExprType::ColumnRef)
        return false;
    const auto & ca = static_cast<const popo::parser::ColumnRefExpr &>(*a);
    const auto & cb = static_cast<const popo::parser::ColumnRefExpr &>(*b);
    return eq_ci(ca.table_name, cb.table_name) && eq_ci(ca.column_name, cb.column_name);
}

bool same_equijoin_expr(const ExprPtr & a, const ExprPtr & b)
{
    if (!a || !b || a->type != ExprType::BinaryOp || b->type != ExprType::BinaryOp)
        return false;
    const auto & ba = static_cast<const popo::parser::BinaryOpExpr &>(*a);
    const auto & bb = static_cast<const popo::parser::BinaryOpExpr &>(*b);
    if (ba.op != BinaryOpType::Eq || bb.op != BinaryOpType::Eq)
        return false;
    return (same_col_ref(ba.left, bb.left) && same_col_ref(ba.right, bb.right))
        || (same_col_ref(ba.left, bb.right) && same_col_ref(ba.right, bb.left));
}

void extract_common_or_equijoins(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr || expr->type != ExprType::BinaryOp)
        return;
    const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
    if (bin.op != BinaryOpType::Or)
        return;
    std::vector<ExprPtr> branches;
    split_or_branches(expr, branches);
    if (branches.size() < 2)
        return;
    std::vector<std::vector<ExprPtr>> branch_conjuncts(branches.size());
    for (size_t i = 0; i < branches.size(); ++i)
        split_and_conjuncts(branches[i], branch_conjuncts[i]);
    for (const auto & cand : branch_conjuncts[0])
    {
        if (!cand || cand->type != ExprType::BinaryOp)
            continue;
        const auto & cb = static_cast<const popo::parser::BinaryOpExpr &>(*cand);
        if (cb.op != BinaryOpType::Eq || !cb.left || !cb.right
            || cb.left->type != ExprType::ColumnRef || cb.right->type != ExprType::ColumnRef)
            continue;
        bool in_all = true;
        for (size_t i = 1; i < branch_conjuncts.size(); ++i)
        {
            bool found = false;
            for (const auto & other : branch_conjuncts[i])
            {
                if (same_equijoin_expr(cand, other))
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                in_all = false;
                break;
            }
        }
        if (in_all)
            out.push_back(cand);
    }
}

ExprPtr combine_and(const ExprPtr & a, const ExprPtr & b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    auto node = std::make_shared<popo::parser::BinaryOpExpr>();
    node->op = BinaryOpType::And;
    node->left = a;
    node->right = b;
    return node;
}

ExprPtr combine_and(const std::vector<ExprPtr> & conjuncts)
{
    if (conjuncts.empty())
        return nullptr;
    ExprPtr res = conjuncts[0];
    for (size_t i = 1; i < conjuncts.size(); ++i)
    {
        auto b = std::make_shared<popo::parser::BinaryOpExpr>();
        b->op = BinaryOpType::And;
        b->left = res;
        b->right = conjuncts[i];
        res = b;
    }
    return res;
}

ExprPtr combine_or(const std::vector<ExprPtr> & branches)
{
    if (branches.empty())
        return nullptr;
    ExprPtr res = branches[0];
    for (size_t i = 1; i < branches.size(); ++i)
    {
        auto b = std::make_shared<popo::parser::BinaryOpExpr>();
        b->op = BinaryOpType::Or;
        b->left = res;
        b->right = branches[i];
        res = b;
    }
    return res;
}

ExprPtr make_literal_expr(Value v)
{
    auto lit = std::make_shared<popo::parser::LiteralExpr>();
    lit->value = std::move(v);
    return lit;
}

bool expr_has_subquery_or_agg(const ExprPtr & expr)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
        case ExprType::Quantified:
        case ExprType::FunctionCall:
        case ExprType::Window:
            return true;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return expr_has_subquery_or_agg(b.left) || expr_has_subquery_or_agg(b.right);
        }
        case ExprType::UnaryOp:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case ExprType::Alias:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::AliasExpr &>(*expr).expr);
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            if (expr_has_subquery_or_agg(in.operand))
                return true;
            for (const auto & item : in.list)
                if (expr_has_subquery_or_agg(item))
                    return true;
            return false;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            return expr_has_subquery_or_agg(b.operand) || expr_has_subquery_or_agg(b.low) || expr_has_subquery_or_agg(b.high);
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            return expr_has_subquery_or_agg(l.operand) || expr_has_subquery_or_agg(l.pattern);
        }
        case ExprType::IsNull:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::IsNullExpr &>(*expr).operand);
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            return expr_has_subquery_or_agg(d.left) || expr_has_subquery_or_agg(d.right);
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            if (expr_has_subquery_or_agg(c.operand) || expr_has_subquery_or_agg(c.else_expr))
                return true;
            for (const auto & [w, t] : c.whens)
                if (expr_has_subquery_or_agg(w) || expr_has_subquery_or_agg(t))
                    return true;
            return false;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
                if (expr_has_subquery_or_agg(a))
                    return true;
            return false;
        }
        case ExprType::Cast:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::CastExpr &>(*expr).operand);
        case ExprType::Interval:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::IntervalExpr &>(*expr).value);
        default:
            return false;
    }
}

void collect_col_refs(const ExprPtr & expr, std::vector<const popo::parser::ColumnRefExpr *> & out)
{
    if (!expr)
        return;
    switch (expr->type)
    {
        case ExprType::ColumnRef:
            out.push_back(&static_cast<const popo::parser::ColumnRefExpr &>(*expr));
            return;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            collect_col_refs(b.left, out);
            collect_col_refs(b.right, out);
            return;
        }
        case ExprType::UnaryOp:
            collect_col_refs(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out);
            return;
        case ExprType::Alias:
            collect_col_refs(static_cast<const popo::parser::AliasExpr &>(*expr).expr, out);
            return;
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            collect_col_refs(in.operand, out);
            for (const auto & item : in.list)
                collect_col_refs(item, out);
            return;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            collect_col_refs(b.operand, out);
            collect_col_refs(b.low, out);
            collect_col_refs(b.high, out);
            return;
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            collect_col_refs(l.operand, out);
            collect_col_refs(l.pattern, out);
            return;
        }
        case ExprType::IsNull:
            collect_col_refs(static_cast<const popo::parser::IsNullExpr &>(*expr).operand, out);
            return;
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            collect_col_refs(d.left, out);
            collect_col_refs(d.right, out);
            return;
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            collect_col_refs(c.operand, out);
            for (const auto & [w, t] : c.whens)
            {
                collect_col_refs(w, out);
                collect_col_refs(t, out);
            }
            collect_col_refs(c.else_expr, out);
            return;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
                collect_col_refs(a, out);
            return;
        }
        case ExprType::Cast:
            collect_col_refs(static_cast<const popo::parser::CastExpr &>(*expr).operand, out);
            return;
        case ExprType::Interval:
            collect_col_refs(static_cast<const popo::parser::IntervalExpr &>(*expr).value, out);
            return;
        default:
            return;
    }
}

ExprPtr fold_constants(const ExprPtr & expr)
{
    if (!expr)
        return nullptr;
    switch (expr->type)
    {
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            ExprPtr l = fold_constants(b.left);
            ExprPtr r = fold_constants(b.right);
            if ((b.op == BinaryOpType::Add || b.op == BinaryOpType::Sub)
                && l && l->type == ExprType::Literal && r && r->type == ExprType::Interval)
            {
                const auto & dval = static_cast<const popo::parser::LiteralExpr &>(*l).value;
                const auto & iv = static_cast<const popo::parser::IntervalExpr &>(*r);
                ExprPtr iv_val = fold_constants(iv.value);
                if (!is_null(dval) && iv_val && iv_val->type == ExprType::Literal)
                {
                    const auto & aval = static_cast<const popo::parser::LiteralExpr &>(*iv_val).value;
                    if (!is_null(aval))
                    {
                        int64_t delta = 0;
                        bool ok = true;
                        if (aval.is_numeric())
                            delta = to_i64(aval);
                        else if (aval.datatype() == ValueType::String)
                        {
                            auto p = popo::base::parse_i64(aval.string_ref());
                            if (p)
                                delta = *p;
                            else
                                ok = false;
                        }
                        else
                            ok = false;
                        if (ok)
                        {
                            if (b.op == BinaryOpType::Sub)
                                delta = -delta;
                            std::string dstr = dval.datatype() == ValueType::String ? dval.string_ref() : dval.to_val_string();
                            auto folded = add_date_interval(dstr, delta, iv.unit);
                            if (folded)
                                return make_literal_expr(Value(ValueType::String, *folded));
                        }
                    }
                }
            }
            if (b.op == BinaryOpType::Add
                && l && l->type == ExprType::Interval && r && r->type == ExprType::Literal)
            {
                const auto & iv = static_cast<const popo::parser::IntervalExpr &>(*l);
                const auto & dval = static_cast<const popo::parser::LiteralExpr &>(*r).value;
                ExprPtr iv_val = fold_constants(iv.value);
                if (!is_null(dval) && iv_val && iv_val->type == ExprType::Literal)
                {
                    const auto & aval = static_cast<const popo::parser::LiteralExpr &>(*iv_val).value;
                    if (!is_null(aval))
                    {
                        int64_t delta = 0;
                        bool ok = true;
                        if (aval.is_numeric())
                            delta = to_i64(aval);
                        else if (aval.datatype() == ValueType::String)
                        {
                            auto p = popo::base::parse_i64(aval.string_ref());
                            if (p)
                                delta = *p;
                            else
                                ok = false;
                        }
                        else
                            ok = false;
                        if (ok)
                        {
                            std::string dstr = dval.datatype() == ValueType::String ? dval.string_ref() : dval.to_val_string();
                            auto folded = add_date_interval(dstr, delta, iv.unit);
                            if (folded)
                                return make_literal_expr(Value(ValueType::String, *folded));
                        }
                    }
                }
            }
            if (l && l->type == ExprType::Literal && r && r->type == ExprType::Literal)
            {
                const auto & lv = static_cast<const popo::parser::LiteralExpr &>(*l).value;
                const auto & rv = static_cast<const popo::parser::LiteralExpr &>(*r).value;
                char arith_op = 0;
                if (b.op == BinaryOpType::Add) arith_op = '+';
                else if (b.op == BinaryOpType::Sub) arith_op = '-';
                else if (b.op == BinaryOpType::Mul) arith_op = '*';
                else if (b.op == BinaryOpType::Div) arith_op = '/';
                if (arith_op != 0)
                {
                    auto folded = do_arithmetic(arith_op, lv, rv);
                    if (folded)
                        return make_literal_expr(std::move(*folded));
                }
            }
            if (l == b.left && r == b.right)
                return expr;
            auto copy = std::make_shared<popo::parser::BinaryOpExpr>(b);
            copy->left = std::move(l);
            copy->right = std::move(r);
            return copy;
        }
        case ExprType::UnaryOp: {
            const auto & u = static_cast<const popo::parser::UnaryOpExpr &>(*expr);
            ExprPtr op = fold_constants(u.operand);
            if (u.op == popo::parser::UnaryOpType::Neg && op && op->type == ExprType::Literal)
            {
                const auto & v = static_cast<const popo::parser::LiteralExpr &>(*op).value;
                if (is_null(v))
                    return make_literal_expr(Value());
                if (is_floating(v))
                    return make_literal_expr(Value(ValueType::Float64, -v.to_f64()));
                return make_literal_expr(Value(ValueType::Int64, -to_i64(v)));
            }
            if (op == u.operand)
                return expr;
            auto copy = std::make_shared<popo::parser::UnaryOpExpr>(u);
            copy->operand = std::move(op);
            return copy;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            ExprPtr op = fold_constants(b.operand);
            ExprPtr lo = fold_constants(b.low);
            ExprPtr hi = fold_constants(b.high);
            if (op == b.operand && lo == b.low && hi == b.high)
                return expr;
            auto copy = std::make_shared<popo::parser::BetweenExpr>(b);
            copy->operand = std::move(op);
            copy->low = std::move(lo);
            copy->high = std::move(hi);
            return copy;
        }
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            ExprPtr op = fold_constants(in.operand);
            bool changed = (op != in.operand);
            std::vector<ExprPtr> list;
            list.reserve(in.list.size());
            for (const auto & item : in.list)
            {
                ExprPtr f = fold_constants(item);
                if (f != item)
                    changed = true;
                list.push_back(std::move(f));
            }
            if (!changed)
                return expr;
            auto copy = std::make_shared<popo::parser::InExpr>(in);
            copy->operand = std::move(op);
            copy->list = std::move(list);
            return copy;
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            ExprPtr op = fold_constants(l.operand);
            ExprPtr pat = fold_constants(l.pattern);
            if (op == l.operand && pat == l.pattern)
                return expr;
            auto copy = std::make_shared<popo::parser::LikeExpr>(l);
            copy->operand = std::move(op);
            copy->pattern = std::move(pat);
            return copy;
        }
        case ExprType::IsNull: {
            const auto & isnull = static_cast<const popo::parser::IsNullExpr &>(*expr);
            ExprPtr op = fold_constants(isnull.operand);
            if (op == isnull.operand)
                return expr;
            auto copy = std::make_shared<popo::parser::IsNullExpr>(isnull);
            copy->operand = std::move(op);
            return copy;
        }
        case ExprType::Alias: {
            const auto & a = static_cast<const popo::parser::AliasExpr &>(*expr);
            ExprPtr inner = fold_constants(a.expr);
            if (inner == a.expr)
                return expr;
            auto copy = std::make_shared<popo::parser::AliasExpr>(a);
            copy->expr = std::move(inner);
            return copy;
        }
        case ExprType::FunctionCall: {
            const auto & f = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
            bool changed = false;
            std::vector<ExprPtr> args;
            args.reserve(f.args.size());
            for (const auto & a : f.args)
            {
                ExprPtr fa = fold_constants(a);
                if (fa != a)
                    changed = true;
                args.push_back(std::move(fa));
            }
            if (!changed)
                return expr;
            auto copy = std::make_shared<popo::parser::FunctionCallExpr>(f);
            copy->args = std::move(args);
            return copy;
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            ExprPtr op = fold_constants(c.operand);
            ExprPtr el = fold_constants(c.else_expr);
            bool changed = (op != c.operand || el != c.else_expr);
            std::vector<std::pair<ExprPtr, ExprPtr>> whens;
            whens.reserve(c.whens.size());
            for (const auto & [w, t] : c.whens)
            {
                ExprPtr fw = fold_constants(w);
                ExprPtr ft = fold_constants(t);
                if (fw != w || ft != t)
                    changed = true;
                whens.emplace_back(std::move(fw), std::move(ft));
            }
            if (!changed)
                return expr;
            auto copy = std::make_shared<popo::parser::CaseExpr>(c);
            copy->operand = std::move(op);
            copy->whens = std::move(whens);
            copy->else_expr = std::move(el);
            return copy;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            bool changed = false;
            std::vector<ExprPtr> args;
            args.reserve(sc.args.size());
            for (const auto & a : sc.args)
            {
                ExprPtr fa = fold_constants(a);
                if (fa != a)
                    changed = true;
                args.push_back(std::move(fa));
            }
            if (!changed)
                return expr;
            auto copy = std::make_shared<popo::parser::ScalarCallExpr>(sc);
            copy->args = std::move(args);
            return copy;
        }
        case ExprType::Cast: {
            const auto & ce = static_cast<const popo::parser::CastExpr &>(*expr);
            ExprPtr op = fold_constants(ce.operand);
            if (op == ce.operand)
                return expr;
            auto copy = std::make_shared<popo::parser::CastExpr>(ce);
            copy->operand = std::move(op);
            return copy;
        }
        default:
            return expr;
    }
}

void collect_select_col_refs(
    const popo::parser::SelectStatement & sel,
    std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star)
{
    for (const auto & cte : sel.with_clause)
        if (cte.query)
            collect_select_col_refs(*cte.query, out, saw_star);
    for (const auto & e : sel.select_list)
        collect_col_refs_deep(e, out, saw_star);
    collect_table_ref_col_refs(sel.from, out, saw_star);
    collect_col_refs_deep(sel.where, out, saw_star);
    for (const auto & g : sel.group_by)
        collect_col_refs_deep(g, out, saw_star);
    collect_col_refs_deep(sel.having, out, saw_star);
    for (const auto & o : sel.order_by)
        collect_col_refs_deep(o.expr, out, saw_star);
    for (const auto & st : sel.set_ops)
        if (st.query)
            collect_select_col_refs(*st.query, out, saw_star);
}

void collect_table_ref_col_refs(
    const popo::parser::TableRefPtr & ref,
    std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star)
{
    if (!ref)
        return;
    if (ref->type == popo::parser::TableRefType::Join)
    {
        const auto & j = static_cast<const popo::parser::JoinTableRef &>(*ref);
        collect_table_ref_col_refs(j.left, out, saw_star);
        collect_table_ref_col_refs(j.right, out, saw_star);
        collect_col_refs_deep(j.condition, out, saw_star);
    }
    else if (ref->type == popo::parser::TableRefType::Derived)
    {
        const auto & d = static_cast<const popo::parser::DerivedTableRef &>(*ref);
        if (d.query && d.query->type == popo::parser::StmtType::Select)
            collect_select_col_refs(static_cast<const popo::parser::SelectStatement &>(*d.query), out, saw_star);
    }
}

void collect_col_refs_deep(
    const ExprPtr & expr, std::vector<const popo::parser::ColumnRefExpr *> & out, bool & saw_star)
{
    if (!expr)
        return;
    switch (expr->type)
    {
        case ExprType::Star:
            saw_star = true;
            return;
        case ExprType::ColumnRef:
            out.push_back(&static_cast<const popo::parser::ColumnRefExpr &>(*expr));
            return;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            collect_col_refs_deep(b.left, out, saw_star);
            collect_col_refs_deep(b.right, out, saw_star);
            return;
        }
        case ExprType::UnaryOp:
            collect_col_refs_deep(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out, saw_star);
            return;
        case ExprType::Alias:
            collect_col_refs_deep(static_cast<const popo::parser::AliasExpr &>(*expr).expr, out, saw_star);
            return;
        case ExprType::FunctionCall: {
            const auto & f = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
            for (const auto & a : f.args)
                if (a && a->type != ExprType::Star)
                    collect_col_refs_deep(a, out, saw_star);
            return;
        }
        case ExprType::Window: {
            const auto & w = static_cast<const popo::parser::WindowExpr &>(*expr);
            collect_col_refs_deep(w.function, out, saw_star);
            for (const auto & p : w.partition_by)
                collect_col_refs_deep(p, out, saw_star);
            for (const auto & o : w.order_by)
                collect_col_refs_deep(o.expr, out, saw_star);
            return;
        }
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            collect_col_refs_deep(in.operand, out, saw_star);
            for (const auto & item : in.list)
                collect_col_refs_deep(item, out, saw_star);
            return;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            collect_col_refs_deep(b.operand, out, saw_star);
            collect_col_refs_deep(b.low, out, saw_star);
            collect_col_refs_deep(b.high, out, saw_star);
            return;
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            collect_col_refs_deep(l.operand, out, saw_star);
            collect_col_refs_deep(l.pattern, out, saw_star);
            return;
        }
        case ExprType::IsNull:
            collect_col_refs_deep(static_cast<const popo::parser::IsNullExpr &>(*expr).operand, out, saw_star);
            return;
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            collect_col_refs_deep(d.left, out, saw_star);
            collect_col_refs_deep(d.right, out, saw_star);
            return;
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            collect_col_refs_deep(c.operand, out, saw_star);
            for (const auto & [w, t] : c.whens)
            {
                collect_col_refs_deep(w, out, saw_star);
                collect_col_refs_deep(t, out, saw_star);
            }
            collect_col_refs_deep(c.else_expr, out, saw_star);
            return;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
                collect_col_refs_deep(a, out, saw_star);
            return;
        }
        case ExprType::Cast:
            collect_col_refs_deep(static_cast<const popo::parser::CastExpr &>(*expr).operand, out, saw_star);
            return;
        case ExprType::Interval:
            collect_col_refs_deep(static_cast<const popo::parser::IntervalExpr &>(*expr).value, out, saw_star);
            return;
        case ExprType::Subquery: {
            const auto & s = static_cast<const popo::parser::SubqueryExpr &>(*expr);
            if (s.query && s.query->type == popo::parser::StmtType::Select)
            {
                bool sub_star = false;
                collect_select_col_refs(static_cast<const popo::parser::SelectStatement &>(*s.query), out, sub_star);
            }
            return;
        }
        case ExprType::InSubquery: {
            const auto & s = static_cast<const popo::parser::InSubqueryExpr &>(*expr);
            collect_col_refs_deep(s.operand, out, saw_star);
            if (s.query && s.query->type == popo::parser::StmtType::Select)
            {
                bool sub_star = false;
                collect_select_col_refs(static_cast<const popo::parser::SelectStatement &>(*s.query), out, sub_star);
            }
            return;
        }
        case ExprType::Exists: {
            const auto & s = static_cast<const popo::parser::ExistsExpr &>(*expr);
            if (s.query && s.query->type == popo::parser::StmtType::Select)
            {
                bool sub_star = false;
                collect_select_col_refs(static_cast<const popo::parser::SelectStatement &>(*s.query), out, sub_star);
            }
            return;
        }
        case ExprType::Quantified: {
            const auto & s = static_cast<const popo::parser::QuantifiedExpr &>(*expr);
            collect_col_refs_deep(s.operand, out, saw_star);
            if (s.query && s.query->type == popo::parser::StmtType::Select)
            {
                bool sub_star = false;
                collect_select_col_refs(static_cast<const popo::parser::SelectStatement &>(*s.query), out, sub_star);
            }
            return;
        }
        default:
            return;
    }
}

void collect_table_names_from_ref(
    const popo::parser::TableRefPtr & ref, std::vector<std::string> & names)
{
    if (!ref)
        return;
    if (ref->type == popo::parser::TableRefType::Simple)
    {
        const auto & s = static_cast<const popo::parser::SimpleTableRef &>(*ref);
        names.push_back(s.alias.empty() ? s.table_name : s.alias);
    }
    else if (ref->type == popo::parser::TableRefType::Join)
    {
        const auto & j = static_cast<const popo::parser::JoinTableRef &>(*ref);
        collect_table_names_from_ref(j.left, names);
        collect_table_names_from_ref(j.right, names);
    }
    else if (ref->type == popo::parser::TableRefType::Derived)
    {
        const auto & d = static_cast<const popo::parser::DerivedTableRef &>(*ref);
        if (!d.alias.empty())
            names.push_back(d.alias);
    }
}

bool extract_scan_and_filters(
    const popo::planner::PlanNodePtr & node,
    const popo::planner::ScanNode *& out_scan,
    ExprPtr & out_filter)
{
    using popo::planner::PlanNodeType;
    std::vector<ExprPtr> filters;
    auto cur = node;
    while (cur && cur->type == PlanNodeType::Filter)
    {
        const auto & f = static_cast<const popo::planner::FilterNode &>(*cur);
        if (f.condition)
            filters.push_back(f.condition);
        cur = f.child;
    }
    if (!cur || cur->type != PlanNodeType::Scan)
        return false;
    out_scan = &static_cast<const popo::planner::ScanNode &>(*cur);
    out_filter = nullptr;
    for (auto it = filters.rbegin(); it != filters.rend(); ++it)
        out_filter = out_filter ? combine_and(out_filter, *it) : *it;
    return true;
}

bool mask_has_any_nonzero(const uint8_t * mask, size_t n) noexcept
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w = 0;
        std::memcpy(&w, mask + i, 8);
        if (w != 0)
            return true;
    }
    for (; i < n; ++i)
        if (mask[i])
            return true;
    return false;
}

bool expr_is_pure_integer(
    const ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<ValueType> & col_types)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case ExprType::Alias:
            return expr_is_pure_integer(
                static_cast<const popo::parser::AliasExpr &>(*expr).expr,
                scan_table, scan_alias, cols, col_types);
        case ExprType::Literal: {
            const auto dt = static_cast<const popo::parser::LiteralExpr &>(*expr).value.datatype();
            return dt == ValueType::Int32 || dt == ValueType::Int64;
        }
        case ExprType::ColumnRef: {
            int c = resolve_single_scan_col(
                scan_table, scan_alias, cols,
                static_cast<const popo::parser::ColumnRefExpr &>(*expr));
            if (c < 0)
                return false;
            const auto dt = col_types[static_cast<size_t>(c)];
            return dt == ValueType::Int32 || dt == ValueType::Int64;
        }
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return expr_is_pure_integer(b.left, scan_table, scan_alias, cols, col_types)
                && expr_is_pure_integer(b.right, scan_table, scan_alias, cols, col_types);
        }
        default:
            return false;
    }
}

bool expr_has_int_div(
    const ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<ValueType> & col_types)
{
    if (!expr)
        return false;
    if (expr->type == ExprType::Alias)
        return expr_has_int_div(
            static_cast<const popo::parser::AliasExpr &>(*expr).expr,
            scan_table, scan_alias, cols, col_types);
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (b.op == BinaryOpType::Div
            && expr_is_pure_integer(b.left, scan_table, scan_alias, cols, col_types)
            && expr_is_pure_integer(b.right, scan_table, scan_alias, cols, col_types))
            return true;
        return expr_has_int_div(b.left, scan_table, scan_alias, cols, col_types)
            || expr_has_int_div(b.right, scan_table, scan_alias, cols, col_types);
    }
    return false;
}

void split_join_equi_and_residual(
    const ExprPtr & condition,
    const std::vector<ColumnSchema> & left_schemas,
    const std::vector<ColumnSchema> & right_schemas,
    std::vector<std::pair<ExprPtr, ExprPtr>> & equi_keys,
    ExprPtr & residual_cond)
{
    if (!condition)
        return;
    auto in_schemas = [](const std::vector<ColumnSchema> & schemas, const std::string & tbl, const std::string & col)
    {
        for (const auto & s : schemas)
        {
            bool tm = tbl.empty() || eq_ci(tbl, s.table) || (!s.alias.empty() && eq_ci(tbl, s.alias));
            if (!tm)
                continue;
            for (const auto & c : s.columns)
                if (eq_ci(c, col))
                    return true;
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
            bool in_l = in_schemas(left_schemas, ref->table_name, ref->column_name);
            bool in_r = in_schemas(right_schemas, ref->table_name, ref->column_name);
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
    split_and_conjuncts(condition, parts);
    for (const auto & p : parts)
    {
        if (p && p->type == ExprType::BinaryOp)
        {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*p);
            if (b.op == BinaryOpType::Eq)
            {
                int sl = side_of(b.left);
                int sr = side_of(b.right);
                if (sl == 1 && sr == 2)
                {
                    equi_keys.emplace_back(b.left, b.right);
                    continue;
                }
                if (sl == 2 && sr == 1)
                {
                    equi_keys.emplace_back(b.right, b.left);
                    continue;
                }
            }
        }
        residual_cond = residual_cond ? combine_and(residual_cond, p) : p;
    }
}

bool collect_from_table_schemas(
    const popo::parser::TableRefPtr & ref,
    popo::catalog::SchemaManager & schema_mgr,
    std::vector<ColumnSchema> & out,
    std::vector<ExprPtr> & join_conds)
{
    if (!ref)
        return false;
    switch (ref->type)
    {
        case popo::parser::TableRefType::Simple: {
            const auto & s = static_cast<const popo::parser::SimpleTableRef &>(*ref);
            auto qr = schema_mgr.query_table(s.table_name);
            if (!qr || !*qr)
                return false;
            ColumnSchema cs;
            cs.table = s.alias.empty() ? s.table_name : s.alias;
            cs.alias = s.alias;
            if (!s.column_aliases.empty())
                cs.columns = s.column_aliases;
            else
                for (const auto & col : (*qr)->columns_)
                    cs.columns.push_back(col.column_name_);
            out.push_back(std::move(cs));
            return true;
        }
        case popo::parser::TableRefType::Join: {
            const auto & j = static_cast<const popo::parser::JoinTableRef &>(*ref);
            if (j.condition)
                join_conds.push_back(j.condition);
            return collect_from_table_schemas(j.left, schema_mgr, out, join_conds)
                && collect_from_table_schemas(j.right, schema_mgr, out, join_conds);
        }
        default:
            return false;
    }
}

bool col_resolves_in_schemas(
    const std::vector<ColumnSchema> & schemas,
    const std::string & tbl, const std::string & col)
{
    for (const auto & s : schemas)
    {
        bool tm = tbl.empty() || eq_ci(tbl, s.table) || (!s.alias.empty() && eq_ci(tbl, s.alias));
        if (!tm)
            continue;
        for (const auto & c : s.columns)
            if (eq_ci(c, col))
                return true;
    }
    return false;
}

bool col_resolves_in_env_chain(
    const EvalEnv * env,
    const std::string & tbl, const std::string & col)
{
    for (const auto * cur = env; cur; cur = cur->outer)
    {
        if (cur->layout && col_resolves_in_schemas(cur->layout->schemas, tbl, col))
            return true;
    }
    return false;
}

int classify_corr_expr(
    const ExprPtr & expr,
    const std::vector<ColumnSchema> & inner_schemas,
    const EvalEnv & outer_env)
{
    if (!expr || expr_has_subquery_or_agg(expr))
        return -1;
    std::vector<const popo::parser::ColumnRefExpr *> refs;
    collect_col_refs(expr, refs);
    if (refs.empty())
        return 0;
    bool has_inner = false;
    bool has_outer = false;
    for (const auto * r : refs)
    {
        if (col_resolves_in_schemas(inner_schemas, r->table_name, r->column_name))
            has_inner = true;
        else if (col_resolves_in_env_chain(&outer_env, r->table_name, r->column_name))
            has_outer = true;
        else
            return -1;
    }
    if (has_inner && has_outer)
        return -1;
    return has_inner ? 1 : 2;
}

bool expr_contains_agg_only(const ExprPtr & expr, bool & saw_agg)
{
    if (!expr)
        return true;
    switch (expr->type)
    {
        case ExprType::FunctionCall: {
            saw_agg = true;
            const auto & fn = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
            for (const auto & a : fn.args)
                if (!expr_contains_agg_only(a, saw_agg))
                    return false;
            return true;
        }
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
        case ExprType::Window:
        case ExprType::Quantified:
            return false;
        case ExprType::Alias:
            return expr_contains_agg_only(static_cast<const popo::parser::AliasExpr &>(*expr).expr, saw_agg);
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return expr_contains_agg_only(b.left, saw_agg) && expr_contains_agg_only(b.right, saw_agg);
        }
        case ExprType::UnaryOp:
            return expr_contains_agg_only(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, saw_agg);
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
                if (!expr_contains_agg_only(a, saw_agg))
                    return false;
            return true;
        }
        case ExprType::Cast:
            return expr_contains_agg_only(static_cast<const popo::parser::CastExpr &>(*expr).operand, saw_agg);
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            if (!expr_contains_agg_only(c.operand, saw_agg))
                return false;
            for (const auto & [w, t] : c.whens)
                if (!expr_contains_agg_only(w, saw_agg) || !expr_contains_agg_only(t, saw_agg))
                    return false;
            return expr_contains_agg_only(c.else_expr, saw_agg);
        }
        default:
            return true;
    }
}

void collect_agg_expr_ptrs(const ExprPtr & expr, std::vector<const popo::parser::FunctionCallExpr *> & out)
{
    if (!expr)
        return;
    switch (expr->type)
    {
        case ExprType::FunctionCall:
            out.push_back(&static_cast<const popo::parser::FunctionCallExpr &>(*expr));
            return;
        case ExprType::Alias:
            collect_agg_expr_ptrs(static_cast<const popo::parser::AliasExpr &>(*expr).expr, out);
            return;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            collect_agg_expr_ptrs(b.left, out);
            collect_agg_expr_ptrs(b.right, out);
            return;
        }
        case ExprType::UnaryOp:
            collect_agg_expr_ptrs(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out);
            return;
        case ExprType::ScalarCall:
            for (const auto & a : static_cast<const popo::parser::ScalarCallExpr &>(*expr).args)
                collect_agg_expr_ptrs(a, out);
            return;
        case ExprType::Cast:
            collect_agg_expr_ptrs(static_cast<const popo::parser::CastExpr &>(*expr).operand, out);
            return;
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            collect_agg_expr_ptrs(c.operand, out);
            for (const auto & [w, t] : c.whens)
            {
                collect_agg_expr_ptrs(w, out);
                collect_agg_expr_ptrs(t, out);
            }
            collect_agg_expr_ptrs(c.else_expr, out);
            return;
        }
        default:
            return;
    }
}

} // namespace popo::executor::detail
