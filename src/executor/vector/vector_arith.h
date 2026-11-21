#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "executor/vector/columnar_batch.h"
#include "parser/ast.h"
#include "type/value.h"

namespace popo::executor
{

// Vectorized arithmetic tree (float64) evaluated over a 64B-aligned ColumnarBatch.
struct VectorArith
{
    enum class Kind : uint8_t
    {
        Col,
        Const,
        BinOp,
    };

    Kind kind = Kind::Const;
    size_t col_idx = 0;
    double const_val = 0.0;
    char op = '+';
    std::unique_ptr<VectorArith> left;
    std::unique_ptr<VectorArith> right;
};

std::unique_ptr<VectorArith> compile_vector_arith(
    const popo::parser::ExprPtr & expr,
    const std::string & scan_table, const std::string & scan_alias,
    const std::vector<std::string> & cols,
    const std::vector<popo::type::ValueType> & col_types);

void eval_vector_arith(
    const VectorArith & node, const ColumnarBatch & batch,
    double * out, uint8_t * out_nulls, bool & any_null);

} // namespace popo::executor
