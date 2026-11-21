#pragma once

#include <cstddef>
#include <cstdint>

#include "parser/ast.h"

namespace popo::executor
{

// ---------------- SIMD Kernels (AVX-512 / AVX2 / Scalar dispatch) ----------------

void simd_fill_u8(uint8_t * dst, size_t n, uint8_t val) noexcept;
void simd_and_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept;
void simd_or_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept;
void simd_not_mask(uint8_t * dst, size_t n) noexcept;
void simd_clear_nulls(uint8_t * mask, const uint8_t * nulls, size_t n) noexcept;
size_t simd_mask_to_sel(const uint8_t * mask, size_t n, uint16_t * sel) noexcept;

void simd_cmp_i64_const(
    const int64_t * a, size_t n, int64_t c, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_cmp_f64_const(
    const double * a, size_t n, double c, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_cmp_i64_col(
    const int64_t * a, const int64_t * b, size_t n, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_cmp_f64_col(
    const double * a, const double * b, size_t n, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_between_i64(
    const int64_t * a, size_t n, int64_t lo, int64_t hi, uint8_t * mask) noexcept;
void simd_between_f64(
    const double * a, size_t n, double lo, double hi, uint8_t * mask) noexcept;

void simd_cmp_u8_const(
    const uint8_t * a, size_t n, uint8_t c, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_cmp_str10_const(
    const uint64_t * hi, const uint16_t * lo, size_t n,
    uint64_t c_hi, uint16_t c_lo, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;
void simd_cmp_str10_col(
    const uint64_t * a_hi, const uint16_t * a_lo,
    const uint64_t * b_hi, const uint16_t * b_lo,
    size_t n, popo::parser::BinaryOpType op, uint8_t * mask) noexcept;

// SIMD arithmetic over 64-byte aligned float64 arrays.
void simd_arith_f64_col_col(const double * a, const double * b, size_t n, char op, double * out) noexcept;
void simd_arith_f64_col_const(const double * a, double c, size_t n, char op, double * out) noexcept;
void simd_arith_f64_const_col(double c, const double * b, size_t n, char op, double * out) noexcept;
void simd_cast_i64_to_f64(const int64_t * a, size_t n, double * out) noexcept;

// SIMD reductions.
double simd_sum_f64_sel(const double * a, const uint16_t * sel, size_t sel_n) noexcept;
double simd_sum_f64_masked(const double * a, const uint8_t * mask, size_t n) noexcept;
double simd_sum_f64_all(const double * a, size_t n) noexcept;
int64_t simd_sum_i64_sel(const int64_t * a, const uint16_t * sel, size_t sel_n) noexcept;

// SIMD vectorized hashing of int64 keys.
void simd_hash_i64(const int64_t * keys, size_t n, uint64_t * hashes) noexcept;

} // namespace popo::executor
