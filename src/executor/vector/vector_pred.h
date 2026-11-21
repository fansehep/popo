#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "function/fast_like.h"
#include "parser/ast.h"
#include "type/value.h"

namespace popo::executor
{

// Compiled SIMD-friendly predicate tree evaluated over a ScanNode's ColumnarBatch.
struct VectorPred
{
    enum class Kind : uint8_t
    {
        And,
        Or,
        Not,
        CmpColConst,
        CmpColCol,
        BetweenConst,
        InConst,
        LikeConst,
        IsNull,
        FallbackScalar,
    };

    Kind kind = Kind::FallbackScalar;
    std::vector<VectorPred> children;
    size_t col1 = 0;
    size_t col2 = 0;
    popo::parser::BinaryOpType op = popo::parser::BinaryOpType::Eq;
    bool negated = false;
    popo::type::Value lit1;
    popo::type::Value lit2;
    uint64_t str10_hi = 0;
    uint16_t str10_lo = 0;
    bool lit1_is_str10 = false;
    std::vector<int64_t> in_i64;
    std::vector<double> in_f64;
    std::vector<std::string> in_str;
    popo::function::FastLike fast_like;
    popo::parser::ExprPtr fallback_expr;
};

int resolve_single_scan_col(
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const popo::parser::ColumnRefExpr & ref);

popo::parser::BinaryOpType flip_cmp_op(popo::parser::BinaryOpType op) noexcept;
bool is_cmp_op(popo::parser::BinaryOpType op) noexcept;

VectorPred compile_vector_pred(
    const popo::parser::ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols);

} // namespace popo::executor
