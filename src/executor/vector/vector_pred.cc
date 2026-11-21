#include "executor/vector/vector_pred.h"

#include <algorithm>
#include <string_view>

#include "function/date_functions.h"
#include "function/value_functions.h"

namespace popo::executor
{

using popo::function::eq_ci;
using popo::function::FastLike;
using popo::function::is_null;
using popo::function::pack_str10;
using popo::function::to_i64;
using popo::parser::BinaryOpType;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::ValueType;

namespace
{

void split_and_local(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == BinaryOpType::And)
        {
            split_and_local(bin.left, out);
            split_and_local(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

void split_or_local(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == BinaryOpType::Or)
        {
            split_or_local(bin.left, out);
            split_or_local(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

} // namespace

int resolve_single_scan_col(
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const popo::parser::ColumnRefExpr & ref)
{
    const bool tmatch = ref.table_name.empty() || eq_ci(ref.table_name, scan_table)
        || (!scan_alias.empty() && eq_ci(ref.table_name, scan_alias));
    if (!tmatch)
        return -1;
    for (size_t i = 0; i < cols.size(); ++i)
        if (eq_ci(cols[i], ref.column_name))
            return static_cast<int>(i);
    return -1;
}

BinaryOpType flip_cmp_op(BinaryOpType op) noexcept
{
    switch (op)
    {
        case BinaryOpType::Lt: return BinaryOpType::Gt;
        case BinaryOpType::Le: return BinaryOpType::Ge;
        case BinaryOpType::Gt: return BinaryOpType::Lt;
        case BinaryOpType::Ge: return BinaryOpType::Le;
        default: return op;
    }
}

bool is_cmp_op(BinaryOpType op) noexcept
{
    switch (op)
    {
        case BinaryOpType::Eq:
        case BinaryOpType::Ne:
        case BinaryOpType::Lt:
        case BinaryOpType::Le:
        case BinaryOpType::Gt:
        case BinaryOpType::Ge:
            return true;
        default:
            return false;
    }
}

VectorPred compile_vector_pred(
    const ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols)
{
    VectorPred p;
    p.fallback_expr = expr;
    if (!expr)
        return p;

    switch (expr->type)
    {
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            if (b.op == BinaryOpType::And)
            {
                p.kind = VectorPred::Kind::And;
                std::vector<ExprPtr> parts;
                split_and_local(expr, parts);
                p.children.reserve(parts.size());
                for (const auto & part : parts)
                    p.children.push_back(compile_vector_pred(part, scan_table, scan_alias, cols));
                auto cost_rank = [](VectorPred::Kind k) -> int
                {
                    switch (k)
                    {
                        case VectorPred::Kind::IsNull: return 0;
                        case VectorPred::Kind::CmpColConst: return 1;
                        case VectorPred::Kind::BetweenConst: return 2;
                        case VectorPred::Kind::CmpColCol: return 3;
                        case VectorPred::Kind::InConst: return 4;
                        case VectorPred::Kind::Or: return 5;
                        case VectorPred::Kind::And: return 5;
                        case VectorPred::Kind::LikeConst: return 6;
                        case VectorPred::Kind::Not: return 7;
                        case VectorPred::Kind::FallbackScalar: return 10;
                    }
                    return 10;
                };
                std::stable_sort(
                    p.children.begin(), p.children.end(),
                    [&](const VectorPred & a, const VectorPred & b)
                    { return cost_rank(a.kind) < cost_rank(b.kind); });
                return p;
            }
            if (b.op == BinaryOpType::Or)
            {
                p.kind = VectorPred::Kind::Or;
                std::vector<ExprPtr> parts;
                split_or_local(expr, parts);
                p.children.reserve(parts.size());
                for (const auto & part : parts)
                    p.children.push_back(compile_vector_pred(part, scan_table, scan_alias, cols));
                return p;
            }
            if (is_cmp_op(b.op) && b.left && b.right)
            {
                if (b.left->type == ExprType::ColumnRef && b.right->type == ExprType::Literal)
                {
                    int c = resolve_single_scan_col(
                        scan_table, scan_alias, cols,
                        static_cast<const popo::parser::ColumnRefExpr &>(*b.left));
                    if (c >= 0)
                    {
                        p.kind = VectorPred::Kind::CmpColConst;
                        p.col1 = static_cast<size_t>(c);
                        p.op = b.op;
                        p.lit1 = static_cast<const popo::parser::LiteralExpr &>(*b.right).value;
                        if (p.lit1.datatype() == ValueType::String && p.lit1.string_ref().size() == 10)
                        {
                            pack_str10(p.lit1.string_ref().data(), p.str10_hi, p.str10_lo);
                            p.lit1_is_str10 = true;
                        }
                        return p;
                    }
                }
                if (b.left->type == ExprType::Literal && b.right->type == ExprType::ColumnRef)
                {
                    int c = resolve_single_scan_col(
                        scan_table, scan_alias, cols,
                        static_cast<const popo::parser::ColumnRefExpr &>(*b.right));
                    if (c >= 0)
                    {
                        p.kind = VectorPred::Kind::CmpColConst;
                        p.col1 = static_cast<size_t>(c);
                        p.op = flip_cmp_op(b.op);
                        p.lit1 = static_cast<const popo::parser::LiteralExpr &>(*b.left).value;
                        if (p.lit1.datatype() == ValueType::String && p.lit1.string_ref().size() == 10)
                        {
                            pack_str10(p.lit1.string_ref().data(), p.str10_hi, p.str10_lo);
                            p.lit1_is_str10 = true;
                        }
                        return p;
                    }
                }
                if (b.left->type == ExprType::ColumnRef && b.right->type == ExprType::ColumnRef)
                {
                    int c1 = resolve_single_scan_col(
                        scan_table, scan_alias, cols,
                        static_cast<const popo::parser::ColumnRefExpr &>(*b.left));
                    int c2 = resolve_single_scan_col(
                        scan_table, scan_alias, cols,
                        static_cast<const popo::parser::ColumnRefExpr &>(*b.right));
                    if (c1 >= 0 && c2 >= 0)
                    {
                        p.kind = VectorPred::Kind::CmpColCol;
                        p.col1 = static_cast<size_t>(c1);
                        p.col2 = static_cast<size_t>(c2);
                        p.op = b.op;
                        return p;
                    }
                }
            }
            return p;
        }
        case ExprType::UnaryOp:
            return p;
        case ExprType::Between: {
            const auto & bw = static_cast<const popo::parser::BetweenExpr &>(*expr);
            if (bw.operand && bw.operand->type == ExprType::ColumnRef
                && bw.low && bw.low->type == ExprType::Literal
                && bw.high && bw.high->type == ExprType::Literal)
            {
                int c = resolve_single_scan_col(
                    scan_table, scan_alias, cols,
                    static_cast<const popo::parser::ColumnRefExpr &>(*bw.operand));
                if (c >= 0)
                {
                    p.kind = VectorPred::Kind::BetweenConst;
                    p.col1 = static_cast<size_t>(c);
                    p.negated = bw.negated;
                    p.lit1 = static_cast<const popo::parser::LiteralExpr &>(*bw.low).value;
                    p.lit2 = static_cast<const popo::parser::LiteralExpr &>(*bw.high).value;
                    return p;
                }
            }
            return p;
        }
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            if (in.operand && in.operand->type == ExprType::ColumnRef && !in.list.empty())
            {
                int c = resolve_single_scan_col(
                    scan_table, scan_alias, cols,
                    static_cast<const popo::parser::ColumnRefExpr &>(*in.operand));
                if (c >= 0)
                {
                    bool all_lit = true;
                    bool has_null_item = false;
                    for (const auto & item : in.list)
                    {
                        if (!item || item->type != ExprType::Literal)
                        {
                            all_lit = false;
                            break;
                        }
                        if (is_null(static_cast<const popo::parser::LiteralExpr &>(*item).value))
                            has_null_item = true;
                    }
                    if (all_lit && !has_null_item)
                    {
                        p.kind = VectorPred::Kind::InConst;
                        p.col1 = static_cast<size_t>(c);
                        p.negated = in.negated;
                        for (const auto & item : in.list)
                        {
                            const auto & v = static_cast<const popo::parser::LiteralExpr &>(*item).value;
                            if (v.datatype() == ValueType::String)
                                p.in_str.push_back(v.string_ref());
                            else if (v.is_numeric())
                            {
                                p.in_i64.push_back(to_i64(v));
                                p.in_f64.push_back(v.to_f64());
                            }
                        }
                        return p;
                    }
                }
            }
            return p;
        }
        case ExprType::Like: {
            const auto & lk = static_cast<const popo::parser::LikeExpr &>(*expr);
            if (lk.operand && lk.operand->type == ExprType::ColumnRef
                && lk.pattern && lk.pattern->type == ExprType::Literal)
            {
                int c = resolve_single_scan_col(
                    scan_table, scan_alias, cols,
                    static_cast<const popo::parser::ColumnRefExpr &>(*lk.operand));
                const auto & pat = static_cast<const popo::parser::LiteralExpr &>(*lk.pattern).value;
                if (c >= 0 && !is_null(pat))
                {
                    p.kind = VectorPred::Kind::LikeConst;
                    p.col1 = static_cast<size_t>(c);
                    p.negated = lk.negated;
                    p.fast_like = FastLike::compile(
                        pat.datatype() == ValueType::String
                            ? std::string_view(pat.string_ref())
                            : std::string_view(pat.to_val_string()));
                    return p;
                }
            }
            return p;
        }
        case ExprType::IsNull: {
            const auto & isn = static_cast<const popo::parser::IsNullExpr &>(*expr);
            if (isn.operand && isn.operand->type == ExprType::ColumnRef)
            {
                int c = resolve_single_scan_col(
                    scan_table, scan_alias, cols,
                    static_cast<const popo::parser::ColumnRefExpr &>(*isn.operand));
                if (c >= 0)
                {
                    p.kind = VectorPred::Kind::IsNull;
                    p.col1 = static_cast<size_t>(c);
                    p.negated = isn.negated;
                    return p;
                }
            }
            return p;
        }
        default:
            return p;
    }
}

} // namespace popo::executor
