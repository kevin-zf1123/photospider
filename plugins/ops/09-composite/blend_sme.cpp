// Copyright 2026 Photospider contributors
#include "09-composite/blend_sme.hpp"

#include <arm_sme.h>

#include "09-composite/blend_math.hpp"

namespace ps::plugin_internal::blend {
__arm_locally_streaming unsigned sme_vector_bytes() {
  return svcntb();
}

__arm_locally_streaming void sme_copy(const void* source, void* output,
                                      std::size_t bytes) {
  const auto* s = static_cast<const std::uint8_t*>(source);
  auto* y = static_cast<std::uint8_t*>(output);
  const std::size_t width = svcntb();
  const auto pg = svptrue_b8();
  std::size_t i = 0;
  for (; bytes - i >= 8 * width; i += 8 * width) {
    const auto v0 = svld1_u8(pg, s + i + 0 * width);
    const auto v1 = svld1_u8(pg, s + i + 1 * width);
    const auto v2 = svld1_u8(pg, s + i + 2 * width);
    const auto v3 = svld1_u8(pg, s + i + 3 * width);
    const auto v4 = svld1_u8(pg, s + i + 4 * width);
    const auto v5 = svld1_u8(pg, s + i + 5 * width);
    const auto v6 = svld1_u8(pg, s + i + 6 * width);
    const auto v7 = svld1_u8(pg, s + i + 7 * width);
    asm volatile("" ::: "memory");
    svst1_u8(pg, y + i + 0 * width, v0);
    svst1_u8(pg, y + i + 1 * width, v1);
    svst1_u8(pg, y + i + 2 * width, v2);
    svst1_u8(pg, y + i + 3 * width, v3);
    svst1_u8(pg, y + i + 4 * width, v4);
    svst1_u8(pg, y + i + 5 * width, v5);
    svst1_u8(pg, y + i + 6 * width, v6);
    svst1_u8(pg, y + i + 7 * width, v7);
  }
  for (; i < bytes; i += width) {
    const auto p = svwhilelt_b8_u64(i, bytes);
    svst1_u8(p, y + i, svld1_u8(p, s + i));
  }
}

template <Kind K>
inline svfloat32_t binary_value(svbool_t p, svfloat32_t b,
                                svfloat32_t s) __arm_streaming {
  if constexpr (K == Kind::Add) {
    return svadd_f32_x(p, b, s);
  } else if constexpr (K == Kind::Subtract) {
    return svsub_f32_x(p, b, s);
  } else if constexpr (K == Kind::Difference) {
    return svabs_f32_x(p, svsub_f32_x(p, b, s));
  } else if constexpr (K == Kind::Divide) {
    return svdiv_f32_x(p, b, s);
  } else if constexpr (K == Kind::Multiply || K == Kind::MultiplyUnbounded) {
    return svmul_f32_x(p, b, s);
  } else if constexpr (K == Kind::Darken) {
    return svsel_f32(svcmple_f32(p, b, s), b, s);
  } else {
    static_assert(K == Kind::Lighten);
    return svsel_f32(svcmple_f32(p, s, b), b, s);
  }
}

template <Kind K>
__arm_locally_streaming void binary(const float* b, const float* s, float* y,
                                    std::size_t count) {
  const std::size_t width = svcntw();
  const auto pg = svptrue_b32();
  std::size_t i = 0;
  for (; count - i >= 8 * width; i += 8 * width) {
    const auto v0 = binary_value<K>(pg, svld1_f32(pg, b + i + 0 * width),
                                    svld1_f32(pg, s + i + 0 * width));
    const auto v1 = binary_value<K>(pg, svld1_f32(pg, b + i + 1 * width),
                                    svld1_f32(pg, s + i + 1 * width));
    const auto v2 = binary_value<K>(pg, svld1_f32(pg, b + i + 2 * width),
                                    svld1_f32(pg, s + i + 2 * width));
    const auto v3 = binary_value<K>(pg, svld1_f32(pg, b + i + 3 * width),
                                    svld1_f32(pg, s + i + 3 * width));
    const auto v4 = binary_value<K>(pg, svld1_f32(pg, b + i + 4 * width),
                                    svld1_f32(pg, s + i + 4 * width));
    const auto v5 = binary_value<K>(pg, svld1_f32(pg, b + i + 5 * width),
                                    svld1_f32(pg, s + i + 5 * width));
    const auto v6 = binary_value<K>(pg, svld1_f32(pg, b + i + 6 * width),
                                    svld1_f32(pg, s + i + 6 * width));
    const auto v7 = binary_value<K>(pg, svld1_f32(pg, b + i + 7 * width),
                                    svld1_f32(pg, s + i + 7 * width));
    asm volatile("" ::: "memory");
    svst1_f32(pg, y + i + 0 * width, v0);
    svst1_f32(pg, y + i + 1 * width, v1);
    svst1_f32(pg, y + i + 2 * width, v2);
    svst1_f32(pg, y + i + 3 * width, v3);
    svst1_f32(pg, y + i + 4 * width, v4);
    svst1_f32(pg, y + i + 5 * width, v5);
    svst1_f32(pg, y + i + 6 * width, v6);
    svst1_f32(pg, y + i + 7 * width, v7);
  }
  for (; i < count; i += width) {
    const auto p = svwhilelt_b32_u64(i, count);
    svst1_f32(p, y + i,
              binary_value<K>(p, svld1_f32(p, b + i), svld1_f32(p, s + i)));
  }
}

void sme_binary(Kind kind, const float* background, const float* source,
                float* output, std::size_t count) {
  switch (kind) {
    case Kind::Add:
      return binary<Kind::Add>(background, source, output, count);
    case Kind::Subtract:
      return binary<Kind::Subtract>(background, source, output, count);
    case Kind::Difference:
      return binary<Kind::Difference>(background, source, output, count);
    case Kind::Divide:
      return binary<Kind::Divide>(background, source, output, count);
    case Kind::Multiply:
      return binary<Kind::Multiply>(background, source, output, count);
    case Kind::MultiplyUnbounded:
      return binary<Kind::MultiplyUnbounded>(background, source, output, count);
    case Kind::Darken:
      return binary<Kind::Darken>(background, source, output, count);
    case Kind::Lighten:
      return binary<Kind::Lighten>(background, source, output, count);
    default:
      return;
  }
}

#if defined(PHOTOSPIDER_HAS_BLEND_SME_F64)
__arm_locally_streaming void sme_difference(const double* b, const double* s,
                                            double* y, std::size_t count) {
  const std::size_t width = svcntd();
  const auto p = svptrue_b64();
  std::size_t i = 0;
  for (; count - i >= 8 * width; i += 8 * width) {
    const auto v0 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 0 * width),
                                   svld1_f64(p, s + i + 0 * width)));
    const auto v1 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 1 * width),
                                   svld1_f64(p, s + i + 1 * width)));
    const auto v2 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 2 * width),
                                   svld1_f64(p, s + i + 2 * width)));
    const auto v3 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 3 * width),
                                   svld1_f64(p, s + i + 3 * width)));
    const auto v4 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 4 * width),
                                   svld1_f64(p, s + i + 4 * width)));
    const auto v5 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 5 * width),
                                   svld1_f64(p, s + i + 5 * width)));
    const auto v6 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 6 * width),
                                   svld1_f64(p, s + i + 6 * width)));
    const auto v7 =
        svabs_f64_x(p, svsub_f64_x(p, svld1_f64(p, b + i + 7 * width),
                                   svld1_f64(p, s + i + 7 * width)));
    asm volatile("" ::: "memory");
    svst1_f64(p, y + i + 0 * width, v0);
    svst1_f64(p, y + i + 1 * width, v1);
    svst1_f64(p, y + i + 2 * width, v2);
    svst1_f64(p, y + i + 3 * width, v3);
    svst1_f64(p, y + i + 4 * width, v4);
    svst1_f64(p, y + i + 5 * width, v5);
    svst1_f64(p, y + i + 6 * width, v6);
    svst1_f64(p, y + i + 7 * width, v7);
  }
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b64_u64(i, count);
    svst1_f64(tail, y + i,
              svabs_f64_x(tail, svsub_f64_x(tail, svld1_f64(tail, b + i),
                                            svld1_f64(tail, s + i))));
  }
}
#endif

}  // namespace ps::plugin_internal::blend
