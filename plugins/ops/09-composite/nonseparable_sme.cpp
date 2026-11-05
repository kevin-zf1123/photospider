// Copyright 2026 Photospider contributors
#include <arm_sme.h>
#include <sys/sysctl.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "09-composite/nonseparable_math.hpp"

namespace ps::plugin_internal::nonseparable {
namespace {
template <class T>
constexpr std::array<T, 192> copy_pattern() {
  std::array<T, 192> p{};
  for (std::size_t i = 0; i < p.size(); i += 3)
    p[i] = 1;
  return p;
}
#define PS_NBL_COPY(T, U, BITS, COUNT)                                         \
  __arm_locally_streaming void copy_##U(const void* first, const void* second, \
                                        void* output, std::size_t pixels) {    \
    static constexpr auto pattern = copy_pattern<T>();                         \
    const auto* l = static_cast<const T*>(first);                              \
    const auto* ab = static_cast<const T*>(second);                            \
    auto* y = static_cast<T*>(output);                                         \
    const std::size_t width = COUNT(), count = 3 * pixels;                     \
    const auto pg = svptrue_b##BITS();                                         \
    const auto p0 = svcmpeq_n_##U(pg, svldnt1_##U(pg, pattern.data()), 1);     \
    const auto p1 =                                                            \
        svcmpeq_n_##U(pg, svldnt1_##U(pg, pattern.data() + width), 1);         \
    const auto p2 =                                                            \
        svcmpeq_n_##U(pg, svldnt1_##U(pg, pattern.data() + 2 * width), 1);     \
    const auto q0 = svnot_b_z(pg, p0), q1 = svnot_b_z(pg, p1),                 \
               q2 = svnot_b_z(pg, p2);                                         \
    std::size_t i = 0;                                                         \
    for (; i + 3 * width <= count; i += 3 * width) {                           \
      const auto a =                                                           \
          svorr_##U##_x(pg, svldnt1_##U(p0, l + i), svldnt1_##U(q0, ab + i));  \
      const auto b = svorr_##U##_x(pg, svldnt1_##U(p1, l + i + width),         \
                                   svldnt1_##U(q1, ab + i + width));           \
      const auto c = svorr_##U##_x(pg, svldnt1_##U(p2, l + i + 2 * width),     \
                                   svldnt1_##U(q2, ab + i + 2 * width));       \
      svst1_##U(pg, y + i, a);                                                 \
      svst1_##U(pg, y + i + width, b);                                         \
      svst1_##U(pg, y + i + 2 * width, c);                                     \
    }                                                                          \
    for (; i < count; i += 3) {                                                \
      y[i] = l[i];                                                             \
      y[i + 1] = ab[i + 1];                                                    \
      y[i + 2] = ab[i + 2];                                                    \
    }                                                                          \
  }
PS_NBL_COPY(std::uint32_t, u32, 32, svcntw)
PS_NBL_COPY(std::uint64_t, u64, 64, svcntd)
#undef PS_NBL_COPY
}  // namespace

bool sme_copy_available() {
  static const bool available = [] {
    int supported = 0;
    std::size_t bytes = sizeof(supported);
    return sysctlbyname("hw.optional.arm.FEAT_SME", &supported, &bytes, nullptr,
                        0) == 0 &&
           bytes == sizeof(supported) && supported;
  }();
  return available;
}
/** Predicated loads touch precisely L from first and a,b from second.
 * Integer transport preserves every IEEE payload bit without arithmetic.
 */
void copy_sme(const void* l, const void* ab, void* output, std::size_t pixels,
              bool fp32) {
  if (fp32)
    copy_u32(l, ab, output, pixels);
  else
    copy_u64(l, ab, output, pixels);
}
}  // namespace ps::plugin_internal::nonseparable
