#include "executor/vector/vector_arith.h"

#include <cstring>

#include "executor/vector/aligned_buffer.h"
#include "executor/vector/simd_kernels.h"
#include "executor/vector/vector_constants.h"
#include "executor/vector/vector_pred.h"

namespace popo::executor
{

using popo::parser::BinaryOpType;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::ValueType;

std::unique_ptr<VectorArith> compile_vector_arith(
    const ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<ValueType> & col_types)
{
    if (!expr)
        return nullptr;
    switch (expr->type)
    {
        case ExprType::Alias:
            return compile_vector_arith(
                static_cast<const popo::parser::AliasExpr &>(*expr).expr,
                scan_table, scan_alias, cols, col_types);
        case ExprType::Literal: {
            const auto & v = static_cast<const popo::parser::LiteralExpr &>(*expr).value;
            if (!v.is_numeric())
                return nullptr;
            auto node = std::make_unique<VectorArith>();
            node->kind = VectorArith::Kind::Const;
            node->const_val = v.to_f64();
            return node;
        }
        case ExprType::ColumnRef: {
            int c = resolve_single_scan_col(
                scan_table, scan_alias, cols,
                static_cast<const popo::parser::ColumnRefExpr &>(*expr));
            if (c < 0)
                return nullptr;
            const auto k = col_kind_for(col_types[static_cast<size_t>(c)]);
            if (k != ColKind::Float64 && k != ColKind::Int64)
                return nullptr;
            auto node = std::make_unique<VectorArith>();
            node->kind = VectorArith::Kind::Col;
            node->col_idx = static_cast<size_t>(c);
            return node;
        }
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            char op = 0;
            if (b.op == BinaryOpType::Add) op = '+';
            else if (b.op == BinaryOpType::Sub) op = '-';
            else if (b.op == BinaryOpType::Mul) op = '*';
            else if (b.op == BinaryOpType::Div) op = '/';
            else return nullptr;
            auto l = compile_vector_arith(b.left, scan_table, scan_alias, cols, col_types);
            auto r = compile_vector_arith(b.right, scan_table, scan_alias, cols, col_types);
            if (!l || !r)
                return nullptr;
            auto node = std::make_unique<VectorArith>();
            node->kind = VectorArith::Kind::BinOp;
            node->op = op;
            node->left = std::move(l);
            node->right = std::move(r);
            return node;
        }
        default:
            return nullptr;
    }
}

void eval_vector_arith(
    const VectorArith & node, const ColumnarBatch & batch,
    double * out, uint8_t * out_nulls, bool & any_null)
{
    const size_t n = batch.count;
    switch (node.kind)
    {
        case VectorArith::Kind::Const:
            for (size_t i = 0; i < n; ++i)
                out[i] = node.const_val;
            if (out_nulls)
                std::memset(out_nulls, 0, n);
            any_null = false;
            return;
        case VectorArith::Kind::Col: {
            const auto & col = batch.columns[node.col_idx];
            if (col.kind == ColKind::Float64)
                std::memcpy(out, col.f64_data.data(), n * sizeof(double));
            else
                simd_cast_i64_to_f64(col.i64_data.data(), n, out);
            any_null = col.has_nulls;
            if (out_nulls)
            {
                if (col.has_nulls)
                    std::memcpy(out_nulls, col.nulls.data(), n);
                else
                    std::memset(out_nulls, 0, n);
            }
            return;
        }
        case VectorArith::Kind::BinOp: {
            if (node.left->kind == VectorArith::Kind::Col && node.right->kind == VectorArith::Kind::Const)
            {
                const auto & col = batch.columns[node.left->col_idx];
                if (col.kind == ColKind::Float64)
                    simd_arith_f64_col_const(col.f64_data.data(), node.right->const_val, n, node.op, out);
                else
                {
                    simd_cast_i64_to_f64(col.i64_data.data(), n, out);
                    simd_arith_f64_col_const(out, node.right->const_val, n, node.op, out);
                }
                any_null = col.has_nulls || (node.op == '/' && node.right->const_val == 0.0);
                if (out_nulls)
                {
                    if (node.op == '/' && node.right->const_val == 0.0)
                        std::memset(out_nulls, 1, n);
                    else if (col.has_nulls)
                        std::memcpy(out_nulls, col.nulls.data(), n);
                    else
                        std::memset(out_nulls, 0, n);
                }
                return;
            }
            if (node.left->kind == VectorArith::Kind::Const && node.right->kind == VectorArith::Kind::Col)
            {
                const auto & col = batch.columns[node.right->col_idx];
                if (col.kind == ColKind::Float64)
                    simd_arith_f64_const_col(node.left->const_val, col.f64_data.data(), n, node.op, out);
                else
                {
                    simd_cast_i64_to_f64(col.i64_data.data(), n, out);
                    simd_arith_f64_const_col(node.left->const_val, out, n, node.op, out);
                }
                any_null = col.has_nulls;
                if (out_nulls)
                {
                    if (col.has_nulls)
                        std::memcpy(out_nulls, col.nulls.data(), n);
                    else
                        std::memset(out_nulls, 0, n);
                }
                return;
            }
            if (node.left->kind == VectorArith::Kind::Col && node.right->kind == VectorArith::Kind::Col)
            {
                const auto & cl = batch.columns[node.left->col_idx];
                const auto & cr = batch.columns[node.right->col_idx];
                if (cl.kind == ColKind::Float64 && cr.kind == ColKind::Float64)
                {
                    simd_arith_f64_col_col(cl.f64_data.data(), cr.f64_data.data(), n, node.op, out);
                    any_null = cl.has_nulls || cr.has_nulls;
                    if (out_nulls)
                    {
                        if (!any_null)
                            std::memset(out_nulls, 0, n);
                        else
                            for (size_t i = 0; i < n; ++i)
                                out_nulls[i] = static_cast<uint8_t>(cl.nulls[i] | cr.nulls[i]);
                    }
                    return;
                }
            }
            AlignedBuffer<double> lbuf(n);
            AlignedBuffer<double> rbuf(n);
            AlignedBuffer<uint8_t> lnull(n);
            AlignedBuffer<uint8_t> rnull(n);
            bool ln = false;
            bool rn = false;
            eval_vector_arith(*node.left, batch, lbuf.data(), out_nulls ? lnull.data() : nullptr, ln);
            eval_vector_arith(*node.right, batch, rbuf.data(), out_nulls ? rnull.data() : nullptr, rn);
            simd_arith_f64_col_col(lbuf.data(), rbuf.data(), n, node.op, out);
            any_null = ln || rn;
            if (out_nulls)
            {
                if (!any_null)
                    std::memset(out_nulls, 0, n);
                else
                    for (size_t i = 0; i < n; ++i)
                        out_nulls[i] = static_cast<uint8_t>(lnull[i] | rnull[i]);
            }
            return;
        }
    }
}

} // namespace popo::executor
