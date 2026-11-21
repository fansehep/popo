#include "executor/vector/simd_kernels.h"

#include <bit>
#include <cstring>

#include "executor/vector/hash_util.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define POPO_X86_SIMD 1
#else
#define POPO_X86_SIMD 0
#endif

namespace popo::executor
{

using popo::parser::BinaryOpType;

namespace
{

#if POPO_X86_SIMD
enum class SimdLevel : uint8_t
{
    Scalar = 0,
    Avx2 = 1,
    Avx512 = 2,
};

SimdLevel detect_simd_level() noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq")
        && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl"))
    {
        return SimdLevel::Avx512;
    }
    if (__builtin_cpu_supports("avx2"))
    {
        return SimdLevel::Avx2;
    }
#endif
    return SimdLevel::Scalar;
}

inline SimdLevel simd_level() noexcept
{
    static const SimdLevel level = detect_simd_level();
    return level;
}
#endif

// ---------------- AVX-512 Kernels ----------------
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_and_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        __m512i a = _mm512_loadu_si512(reinterpret_cast<const void *>(dst + i));
        __m512i b = _mm512_loadu_si512(reinterpret_cast<const void *>(src + i));
        _mm512_storeu_si512(reinterpret_cast<void *>(dst + i), _mm512_and_si512(a, b));
    }
    for (; i < n; ++i)
        dst[i] &= src[i];
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_or_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        __m512i a = _mm512_loadu_si512(reinterpret_cast<const void *>(dst + i));
        __m512i b = _mm512_loadu_si512(reinterpret_cast<const void *>(src + i));
        _mm512_storeu_si512(reinterpret_cast<void *>(dst + i), _mm512_or_si512(a, b));
    }
    for (; i < n; ++i)
        dst[i] |= src[i];
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_not_mask(uint8_t * dst, size_t n) noexcept
{
    const __m512i zero = _mm512_setzero_si512();
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        __m512i a = _mm512_loadu_si512(reinterpret_cast<const void *>(dst + i));
        __mmask64 is_zero = _mm512_cmpeq_epu8_mask(a, zero);
        _mm512_storeu_si512(reinterpret_cast<void *>(dst + i), _mm512_movm_epi8(is_zero));
    }
    for (; i < n; ++i)
        dst[i] = dst[i] ? 0 : 0xFF;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_clear_nulls(uint8_t * mask, const uint8_t * nulls, size_t n) noexcept
{
    const __m512i zero = _mm512_setzero_si512();
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        __m512i nv = _mm512_loadu_si512(reinterpret_cast<const void *>(nulls + i));
        __mmask64 not_null = _mm512_cmpeq_epu8_mask(nv, zero);
        __m512i mv = _mm512_loadu_si512(reinterpret_cast<const void *>(mask + i));
        __m512i out = _mm512_and_si512(mv, _mm512_movm_epi8(not_null));
        _mm512_storeu_si512(reinterpret_cast<void *>(mask + i), out);
    }
    for (; i < n; ++i)
    {
        if (nulls[i])
            mask[i] = 0;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
size_t avx512_mask_to_sel(const uint8_t * mask, size_t n, uint16_t * sel) noexcept
{
    const __m512i zero = _mm512_setzero_si512();
    size_t out = 0;
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
    {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void *>(mask + i));
        uint64_t bits = static_cast<uint64_t>(_mm512_cmpneq_epu8_mask(v, zero));
        while (bits != 0)
        {
            unsigned tz = static_cast<unsigned>(std::countr_zero(bits));
            sel[out++] = static_cast<uint16_t>(i + tz);
            bits &= (bits - 1);
        }
    }
    for (; i < n; ++i)
    {
        if (mask[i])
            sel[out++] = static_cast<uint16_t>(i);
    }
    return out;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
inline void avx512_store_mask8(uint8_t * mask, size_t idx, __mmask8 m) noexcept
{
    _mm_storel_epi64(reinterpret_cast<__m128i *>(mask + idx), _mm_movm_epi8(static_cast<__mmask16>(m)));
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cmp_i64_const(
    const int64_t * a, size_t n, int64_t c, BinaryOpType op, uint8_t * mask) noexcept
{
    const __m512i vc = _mm512_set1_epi64(c);
    size_t i = 0;
    switch (op)
    {
        case BinaryOpType::Eq:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_EQ));
            break;
        case BinaryOpType::Ne:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_NE));
            break;
        case BinaryOpType::Lt:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_LT));
            break;
        case BinaryOpType::Le:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_LE));
            break;
        case BinaryOpType::Gt:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_NLE));
            break;
        case BinaryOpType::Ge:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_epi64_mask(_mm512_loadu_si512(a + i), vc, _MM_CMPINT_NLT));
            break;
        default:
            break;
    }
    for (; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cmp_f64_const(
    const double * a, size_t n, double c, BinaryOpType op, uint8_t * mask) noexcept
{
    const __m512d vc = _mm512_set1_pd(c);
    size_t i = 0;
    switch (op)
    {
        case BinaryOpType::Eq:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_EQ_OQ));
            break;
        case BinaryOpType::Ne:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_NEQ_OQ));
            break;
        case BinaryOpType::Lt:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_LT_OQ));
            break;
        case BinaryOpType::Le:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_LE_OQ));
            break;
        case BinaryOpType::Gt:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_GT_OQ));
            break;
        case BinaryOpType::Ge:
            for (; i + 8 <= n; i += 8)
                avx512_store_mask8(mask, i, _mm512_cmp_pd_mask(_mm512_loadu_pd(a + i), vc, _CMP_GE_OQ));
            break;
        default:
            break;
    }
    for (; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_between_f64(const double * a, size_t n, double lo, double hi, uint8_t * mask) noexcept
{
    const __m512d vlo = _mm512_set1_pd(lo);
    const __m512d vhi = _mm512_set1_pd(hi);
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m512d v = _mm512_loadu_pd(a + i);
        __mmask8 m = _mm512_cmp_pd_mask(v, vlo, _CMP_GE_OQ) & _mm512_cmp_pd_mask(v, vhi, _CMP_LE_OQ);
        avx512_store_mask8(mask, i, m);
    }
    for (; i < n; ++i)
        mask[i] = (a[i] >= lo && a[i] <= hi) ? 0xFF : 0x00;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_between_i64(const int64_t * a, size_t n, int64_t lo, int64_t hi, uint8_t * mask) noexcept
{
    const __m512i vlo = _mm512_set1_epi64(lo);
    const __m512i vhi = _mm512_set1_epi64(hi);
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m512i v = _mm512_loadu_si512(a + i);
        __mmask8 m = _mm512_cmp_epi64_mask(v, vlo, _MM_CMPINT_NLT) & _mm512_cmp_epi64_mask(v, vhi, _MM_CMPINT_LE);
        avx512_store_mask8(mask, i, m);
    }
    for (; i < n; ++i)
        mask[i] = (a[i] >= lo && a[i] <= hi) ? 0xFF : 0x00;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cmp_u8_const(
    const uint8_t * a, size_t n, uint8_t c, BinaryOpType op, uint8_t * mask) noexcept
{
    const __m512i vc = _mm512_set1_epi8(static_cast<char>(c));
    size_t i = 0;
    switch (op)
    {
        case BinaryOpType::Eq:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmpeq_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        case BinaryOpType::Ne:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmpneq_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        case BinaryOpType::Lt:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmplt_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        case BinaryOpType::Le:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmple_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        case BinaryOpType::Gt:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmpgt_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        case BinaryOpType::Ge:
            for (; i + 64 <= n; i += 64)
            {
                __mmask64 m = _mm512_cmpge_epu8_mask(_mm512_loadu_si512(a + i), vc);
                _mm512_storeu_si512(mask + i, _mm512_movm_epi8(m));
            }
            break;
        default:
            break;
    }
    for (; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cmp_str10_const(
    const uint64_t * hi, const uint16_t * lo, size_t n,
    uint64_t c_hi, uint16_t c_lo, BinaryOpType op, uint8_t * mask) noexcept
{
    const __m512i vchi = _mm512_set1_epi64(static_cast<long long>(c_hi));
    const __m128i vclo = _mm_set1_epi16(static_cast<short>(c_lo));
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m512i vhi = _mm512_loadu_si512(hi + i);
        __m128i vlo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(lo + i));
        __mmask8 eq_hi = _mm512_cmp_epu64_mask(vhi, vchi, _MM_CMPINT_EQ);
        __mmask8 m = 0;
        switch (op)
        {
            case BinaryOpType::Eq:
                m = eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_EQ);
                break;
            case BinaryOpType::Ne:
                m = static_cast<__mmask8>(~(eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_EQ)));
                break;
            case BinaryOpType::Lt:
                m = _mm512_cmp_epu64_mask(vhi, vchi, _MM_CMPINT_LT)
                    | (eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_LT));
                break;
            case BinaryOpType::Le:
                m = _mm512_cmp_epu64_mask(vhi, vchi, _MM_CMPINT_LT)
                    | (eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_LE));
                break;
            case BinaryOpType::Gt:
                m = _mm512_cmp_epu64_mask(vhi, vchi, _MM_CMPINT_NLE)
                    | (eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_NLE));
                break;
            case BinaryOpType::Ge:
                m = _mm512_cmp_epu64_mask(vhi, vchi, _MM_CMPINT_NLE)
                    | (eq_hi & _mm_cmp_epu16_mask(vlo, vclo, _MM_CMPINT_NLT));
                break;
            default:
                break;
        }
        avx512_store_mask8(mask, i, m);
    }
    for (; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = (hi[i] == c_hi && lo[i] == c_lo); break;
            case BinaryOpType::Ne: ok = (hi[i] != c_hi || lo[i] != c_lo); break;
            case BinaryOpType::Lt: ok = (hi[i] < c_hi) || (hi[i] == c_hi && lo[i] < c_lo); break;
            case BinaryOpType::Le: ok = (hi[i] < c_hi) || (hi[i] == c_hi && lo[i] <= c_lo); break;
            case BinaryOpType::Gt: ok = (hi[i] > c_hi) || (hi[i] == c_hi && lo[i] > c_lo); break;
            case BinaryOpType::Ge: ok = (hi[i] > c_hi) || (hi[i] == c_hi && lo[i] >= c_lo); break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cmp_str10_col(
    const uint64_t * a_hi, const uint16_t * a_lo,
    const uint64_t * b_hi, const uint16_t * b_lo,
    size_t n, BinaryOpType op, uint8_t * mask) noexcept
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m512i ahi = _mm512_loadu_si512(a_hi + i);
        __m512i bhi = _mm512_loadu_si512(b_hi + i);
        __m128i alo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a_lo + i));
        __m128i blo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b_lo + i));
        __mmask8 eq_hi = _mm512_cmp_epu64_mask(ahi, bhi, _MM_CMPINT_EQ);
        __mmask8 m = 0;
        switch (op)
        {
            case BinaryOpType::Eq:
                m = eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_EQ);
                break;
            case BinaryOpType::Ne:
                m = static_cast<__mmask8>(~(eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_EQ)));
                break;
            case BinaryOpType::Lt:
                m = _mm512_cmp_epu64_mask(ahi, bhi, _MM_CMPINT_LT)
                    | (eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_LT));
                break;
            case BinaryOpType::Le:
                m = _mm512_cmp_epu64_mask(ahi, bhi, _MM_CMPINT_LT)
                    | (eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_LE));
                break;
            case BinaryOpType::Gt:
                m = _mm512_cmp_epu64_mask(ahi, bhi, _MM_CMPINT_NLE)
                    | (eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_NLE));
                break;
            case BinaryOpType::Ge:
                m = _mm512_cmp_epu64_mask(ahi, bhi, _MM_CMPINT_NLE)
                    | (eq_hi & _mm_cmp_epu16_mask(alo, blo, _MM_CMPINT_NLT));
                break;
            default:
                break;
        }
        avx512_store_mask8(mask, i, m);
    }
    for (; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = (a_hi[i] == b_hi[i] && a_lo[i] == b_lo[i]); break;
            case BinaryOpType::Ne: ok = (a_hi[i] != b_hi[i] || a_lo[i] != b_lo[i]); break;
            case BinaryOpType::Lt: ok = (a_hi[i] < b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] < b_lo[i]); break;
            case BinaryOpType::Le: ok = (a_hi[i] < b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] <= b_lo[i]); break;
            case BinaryOpType::Gt: ok = (a_hi[i] > b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] > b_lo[i]); break;
            case BinaryOpType::Ge: ok = (a_hi[i] > b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] >= b_lo[i]); break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_arith_f64_col_col(const double * a, const double * b, size_t n, char op, double * out) noexcept
{
    size_t i = 0;
    switch (op)
    {
        case '+':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_add_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] + b[i];
            break;
        case '-':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_sub_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] - b[i];
            break;
        case '*':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_mul_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] * b[i];
            break;
        case '/':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_div_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = b[i] != 0.0 ? a[i] / b[i] : 0.0;
            break;
        default:
            break;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_arith_f64_col_const(const double * a, double c, size_t n, char op, double * out) noexcept
{
    const __m512d vc = _mm512_set1_pd(c);
    size_t i = 0;
    switch (op)
    {
        case '+':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_add_pd(_mm512_loadu_pd(a + i), vc));
            for (; i < n; ++i) out[i] = a[i] + c;
            break;
        case '-':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_sub_pd(_mm512_loadu_pd(a + i), vc));
            for (; i < n; ++i) out[i] = a[i] - c;
            break;
        case '*':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_mul_pd(_mm512_loadu_pd(a + i), vc));
            for (; i < n; ++i) out[i] = a[i] * c;
            break;
        case '/':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_div_pd(_mm512_loadu_pd(a + i), vc));
            for (; i < n; ++i) out[i] = c != 0.0 ? a[i] / c : 0.0;
            break;
        default:
            break;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_arith_f64_const_col(double c, const double * b, size_t n, char op, double * out) noexcept
{
    const __m512d vc = _mm512_set1_pd(c);
    size_t i = 0;
    switch (op)
    {
        case '+':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_add_pd(vc, _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = c + b[i];
            break;
        case '-':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_sub_pd(vc, _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = c - b[i];
            break;
        case '*':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_mul_pd(vc, _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = c * b[i];
            break;
        case '/':
            for (; i + 8 <= n; i += 8)
                _mm512_storeu_pd(out + i, _mm512_div_pd(vc, _mm512_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = b[i] != 0.0 ? c / b[i] : 0.0;
            break;
        default:
            break;
    }
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_cast_i64_to_f64(const int64_t * a, size_t n, double * out) noexcept
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        _mm512_storeu_pd(out + i, _mm512_cvtepi64_pd(_mm512_loadu_si512(a + i)));
    for (; i < n; ++i)
        out[i] = static_cast<double>(a[i]);
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
double avx512_sum_f64_masked(const double * a, const uint8_t * mask, size_t n) noexcept
{
    __m512d acc = _mm512_setzero_pd();
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m128i m8 = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(mask + i));
        __mmask8 k = static_cast<__mmask8>(_mm_cmpneq_epu8_mask(m8, zero) & 0xFFu);
        if (k != 0)
            acc = _mm512_mask_add_pd(acc, k, acc, _mm512_loadu_pd(a + i));
    }
    double sum = _mm512_reduce_add_pd(acc);
    for (; i < n; ++i)
    {
        if (mask[i])
            sum += a[i];
    }
    return sum;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
double avx512_sum_f64_all(const double * a, size_t n) noexcept
{
    __m512d acc0 = _mm512_setzero_pd();
    __m512d acc1 = _mm512_setzero_pd();
    size_t i = 0;
    for (; i + 16 <= n; i += 16)
    {
        acc0 = _mm512_add_pd(acc0, _mm512_loadu_pd(a + i));
        acc1 = _mm512_add_pd(acc1, _mm512_loadu_pd(a + i + 8));
    }
    acc0 = _mm512_add_pd(acc0, acc1);
    for (; i + 8 <= n; i += 8)
        acc0 = _mm512_add_pd(acc0, _mm512_loadu_pd(a + i));
    double sum = _mm512_reduce_add_pd(acc0);
    for (; i < n; ++i)
        sum += a[i];
    return sum;
}

__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
void avx512_hash_i64(const int64_t * keys, size_t n, uint64_t * hashes) noexcept
{
    const __m512i c1 = _mm512_set1_epi64(static_cast<long long>(0xff51afd7ed558ccdULL));
    const __m512i c2 = _mm512_set1_epi64(static_cast<long long>(0xc4ceb9fe1a85ec53ULL));
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m512i h = _mm512_loadu_si512(keys + i);
        h = _mm512_xor_si512(h, _mm512_srli_epi64(h, 33));
        h = _mm512_mullo_epi64(h, c1);
        h = _mm512_xor_si512(h, _mm512_srli_epi64(h, 33));
        h = _mm512_mullo_epi64(h, c2);
        h = _mm512_xor_si512(h, _mm512_srli_epi64(h, 33));
        _mm512_storeu_si512(hashes + i, h);
    }
    for (; i < n; ++i)
        hashes[i] = hash_i64_scalar(keys[i]);
}

// ---------------- AVX2 Kernels ----------------

__attribute__((target("avx2,fma")))
void avx2_and_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(dst + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), _mm256_and_si256(a, b));
    }
    for (; i < n; ++i)
        dst[i] &= src[i];
}

__attribute__((target("avx2,fma")))
void avx2_or_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(dst + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), _mm256_or_si256(a, b));
    }
    for (; i < n; ++i)
        dst[i] |= src[i];
}

__attribute__((target("avx2,fma")))
size_t avx2_mask_to_sel(const uint8_t * mask, size_t n, uint16_t * sel) noexcept
{
    const __m256i zero = _mm256_setzero_si256();
    size_t out = 0;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(mask + i));
        uint32_t bits = ~static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero)));
        while (bits != 0)
        {
            unsigned tz = static_cast<unsigned>(std::countr_zero(bits));
            sel[out++] = static_cast<uint16_t>(i + tz);
            bits &= (bits - 1);
        }
    }
    for (; i < n; ++i)
    {
        if (mask[i])
            sel[out++] = static_cast<uint16_t>(i);
    }
    return out;
}

__attribute__((target("avx2,fma")))
void avx2_arith_f64_col_col(const double * a, const double * b, size_t n, char op, double * out) noexcept
{
    size_t i = 0;
    switch (op)
    {
        case '+':
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(out + i, _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] + b[i];
            break;
        case '-':
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(out + i, _mm256_sub_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] - b[i];
            break;
        case '*':
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(out + i, _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = a[i] * b[i];
            break;
        case '/':
            for (; i + 4 <= n; i += 4)
                _mm256_storeu_pd(out + i, _mm256_div_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
            for (; i < n; ++i) out[i] = b[i] != 0.0 ? a[i] / b[i] : 0.0;
            break;
        default:
            break;
    }
}

__attribute__((target("avx2,fma")))
double avx2_sum_f64_all(const double * a, size_t n) noexcept
{
    __m256d acc = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm256_add_pd(acc, _mm256_loadu_pd(a + i));
    alignas(32) double lanes[4];
    _mm256_store_pd(lanes, acc);
    double sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    for (; i < n; ++i)
        sum += a[i];
    return sum;
}

#endif // POPO_X86_SIMD

} // namespace

// ---------------- SIMD Dispatch Wrappers ----------------

void simd_fill_u8(uint8_t * dst, size_t n, uint8_t val) noexcept
{
    if (n > 0)
        std::memset(dst, val, n);
}

void simd_and_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    const auto lvl = simd_level();
    if (lvl == SimdLevel::Avx512) { avx512_and_masks(dst, src, n); return; }
    if (lvl == SimdLevel::Avx2) { avx2_and_masks(dst, src, n); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        dst[i] &= src[i];
}

void simd_or_masks(uint8_t * dst, const uint8_t * src, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    const auto lvl = simd_level();
    if (lvl == SimdLevel::Avx512) { avx512_or_masks(dst, src, n); return; }
    if (lvl == SimdLevel::Avx2) { avx2_or_masks(dst, src, n); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        dst[i] |= src[i];
}

void simd_not_mask(uint8_t * dst, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_not_mask(dst, n); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        dst[i] = dst[i] ? 0 : 0xFF;
}

void simd_clear_nulls(uint8_t * mask, const uint8_t * nulls, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_clear_nulls(mask, nulls, n); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        if (nulls[i])
            mask[i] = 0;
    }
}

size_t simd_mask_to_sel(const uint8_t * mask, size_t n, uint16_t * sel) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    const auto lvl = simd_level();
    if (lvl == SimdLevel::Avx512) return avx512_mask_to_sel(mask, n, sel);
    if (lvl == SimdLevel::Avx2) return avx2_mask_to_sel(mask, n, sel);
#endif
    size_t out = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (mask[i])
            sel[out++] = static_cast<uint16_t>(i);
    }
    return out;
}

void simd_cmp_i64_const(
    const int64_t * a, size_t n, int64_t c, BinaryOpType op, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cmp_i64_const(a, n, c, op, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_cmp_f64_const(
    const double * a, size_t n, double c, BinaryOpType op, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cmp_f64_const(a, n, c, op, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_cmp_i64_col(
    const int64_t * a, const int64_t * b, size_t n, BinaryOpType op, uint8_t * mask) noexcept
{
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == b[i]; break;
            case BinaryOpType::Ne: ok = a[i] != b[i]; break;
            case BinaryOpType::Lt: ok = a[i] < b[i]; break;
            case BinaryOpType::Le: ok = a[i] <= b[i]; break;
            case BinaryOpType::Gt: ok = a[i] > b[i]; break;
            case BinaryOpType::Ge: ok = a[i] >= b[i]; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_cmp_f64_col(
    const double * a, const double * b, size_t n, BinaryOpType op, uint8_t * mask) noexcept
{
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == b[i]; break;
            case BinaryOpType::Ne: ok = a[i] != b[i]; break;
            case BinaryOpType::Lt: ok = a[i] < b[i]; break;
            case BinaryOpType::Le: ok = a[i] <= b[i]; break;
            case BinaryOpType::Gt: ok = a[i] > b[i]; break;
            case BinaryOpType::Ge: ok = a[i] >= b[i]; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_between_i64(const int64_t * a, size_t n, int64_t lo, int64_t hi, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_between_i64(a, n, lo, hi, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        mask[i] = (a[i] >= lo && a[i] <= hi) ? 0xFF : 0x00;
}

void simd_between_f64(const double * a, size_t n, double lo, double hi, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_between_f64(a, n, lo, hi, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        mask[i] = (a[i] >= lo && a[i] <= hi) ? 0xFF : 0x00;
}

void simd_cmp_u8_const(
    const uint8_t * a, size_t n, uint8_t c, BinaryOpType op, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cmp_u8_const(a, n, c, op, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = a[i] == c; break;
            case BinaryOpType::Ne: ok = a[i] != c; break;
            case BinaryOpType::Lt: ok = a[i] < c; break;
            case BinaryOpType::Le: ok = a[i] <= c; break;
            case BinaryOpType::Gt: ok = a[i] > c; break;
            case BinaryOpType::Ge: ok = a[i] >= c; break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_cmp_str10_const(
    const uint64_t * hi, const uint16_t * lo, size_t n,
    uint64_t c_hi, uint16_t c_lo, BinaryOpType op, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cmp_str10_const(hi, lo, n, c_hi, c_lo, op, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = (hi[i] == c_hi && lo[i] == c_lo); break;
            case BinaryOpType::Ne: ok = (hi[i] != c_hi || lo[i] != c_lo); break;
            case BinaryOpType::Lt: ok = (hi[i] < c_hi) || (hi[i] == c_hi && lo[i] < c_lo); break;
            case BinaryOpType::Le: ok = (hi[i] < c_hi) || (hi[i] == c_hi && lo[i] <= c_lo); break;
            case BinaryOpType::Gt: ok = (hi[i] > c_hi) || (hi[i] == c_hi && lo[i] > c_lo); break;
            case BinaryOpType::Ge: ok = (hi[i] > c_hi) || (hi[i] == c_hi && lo[i] >= c_lo); break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_cmp_str10_col(
    const uint64_t * a_hi, const uint16_t * a_lo,
    const uint64_t * b_hi, const uint16_t * b_lo,
    size_t n, BinaryOpType op, uint8_t * mask) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cmp_str10_col(a_hi, a_lo, b_hi, b_lo, n, op, mask); return; }
#endif
    for (size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        switch (op)
        {
            case BinaryOpType::Eq: ok = (a_hi[i] == b_hi[i] && a_lo[i] == b_lo[i]); break;
            case BinaryOpType::Ne: ok = (a_hi[i] != b_hi[i] || a_lo[i] != b_lo[i]); break;
            case BinaryOpType::Lt: ok = (a_hi[i] < b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] < b_lo[i]); break;
            case BinaryOpType::Le: ok = (a_hi[i] < b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] <= b_lo[i]); break;
            case BinaryOpType::Gt: ok = (a_hi[i] > b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] > b_lo[i]); break;
            case BinaryOpType::Ge: ok = (a_hi[i] > b_hi[i]) || (a_hi[i] == b_hi[i] && a_lo[i] >= b_lo[i]); break;
            default: break;
        }
        mask[i] = ok ? 0xFF : 0x00;
    }
}

void simd_arith_f64_col_col(const double * a, const double * b, size_t n, char op, double * out) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    const auto lvl = simd_level();
    if (lvl == SimdLevel::Avx512) { avx512_arith_f64_col_col(a, b, n, op, out); return; }
    if (lvl == SimdLevel::Avx2) { avx2_arith_f64_col_col(a, b, n, op, out); return; }
#endif
    switch (op)
    {
        case '+': for (size_t i = 0; i < n; ++i) out[i] = a[i] + b[i]; break;
        case '-': for (size_t i = 0; i < n; ++i) out[i] = a[i] - b[i]; break;
        case '*': for (size_t i = 0; i < n; ++i) out[i] = a[i] * b[i]; break;
        case '/': for (size_t i = 0; i < n; ++i) out[i] = b[i] != 0.0 ? a[i] / b[i] : 0.0; break;
        default: break;
    }
}

void simd_arith_f64_col_const(const double * a, double c, size_t n, char op, double * out) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_arith_f64_col_const(a, c, n, op, out); return; }
#endif
    switch (op)
    {
        case '+': for (size_t i = 0; i < n; ++i) out[i] = a[i] + c; break;
        case '-': for (size_t i = 0; i < n; ++i) out[i] = a[i] - c; break;
        case '*': for (size_t i = 0; i < n; ++i) out[i] = a[i] * c; break;
        case '/': for (size_t i = 0; i < n; ++i) out[i] = c != 0.0 ? a[i] / c : 0.0; break;
        default: break;
    }
}

void simd_arith_f64_const_col(double c, const double * b, size_t n, char op, double * out) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_arith_f64_const_col(c, b, n, op, out); return; }
#endif
    switch (op)
    {
        case '+': for (size_t i = 0; i < n; ++i) out[i] = c + b[i]; break;
        case '-': for (size_t i = 0; i < n; ++i) out[i] = c - b[i]; break;
        case '*': for (size_t i = 0; i < n; ++i) out[i] = c * b[i]; break;
        case '/': for (size_t i = 0; i < n; ++i) out[i] = b[i] != 0.0 ? c / b[i] : 0.0; break;
        default: break;
    }
}

void simd_cast_i64_to_f64(const int64_t * a, size_t n, double * out) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_cast_i64_to_f64(a, n, out); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<double>(a[i]);
}

double simd_sum_f64_masked(const double * a, const uint8_t * mask, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) return avx512_sum_f64_masked(a, mask, n);
#endif
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        if (mask[i])
            sum += a[i];
    }
    return sum;
}

double simd_sum_f64_all(const double * a, size_t n) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    const auto lvl = simd_level();
    if (lvl == SimdLevel::Avx512) return avx512_sum_f64_all(a, n);
    if (lvl == SimdLevel::Avx2) return avx2_sum_f64_all(a, n);
#endif
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i];
        s1 += a[i + 1];
        s2 += a[i + 2];
        s3 += a[i + 3];
    }
    double sum = (s0 + s1) + (s2 + s3);
    for (; i < n; ++i)
        sum += a[i];
    return sum;
}

double simd_sum_f64_sel(const double * a, const uint16_t * sel, size_t sel_n) noexcept
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t i = 0;
    for (; i + 4 <= sel_n; i += 4)
    {
        s0 += a[sel[i]];
        s1 += a[sel[i + 1]];
        s2 += a[sel[i + 2]];
        s3 += a[sel[i + 3]];
    }
    double sum = (s0 + s1) + (s2 + s3);
    for (; i < sel_n; ++i)
        sum += a[sel[i]];
    return sum;
}

int64_t simd_sum_i64_sel(const int64_t * a, const uint16_t * sel, size_t sel_n) noexcept
{
    int64_t sum = 0;
    for (size_t i = 0; i < sel_n; ++i)
        sum += a[sel[i]];
    return sum;
}

void simd_hash_i64(const int64_t * keys, size_t n, uint64_t * hashes) noexcept
{
#if POPO_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (simd_level() == SimdLevel::Avx512) { avx512_hash_i64(keys, n, hashes); return; }
#endif
    for (size_t i = 0; i < n; ++i)
        hashes[i] = hash_i64_scalar(keys[i]);
}

} // namespace popo::executor
