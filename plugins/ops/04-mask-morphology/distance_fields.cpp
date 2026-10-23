#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/spatial.hpp"
#include "common/registry.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::mask {
namespace {
enum class FieldKind { Shift, Threshold, Linear, Smooth };
using Compact = model_ops::RationalMathStorage<32>;
struct FieldScratch final {
  Exact wide{numeric_ops::SequenceProfile::Strict};
  Compact small{numeric_ops::SequenceProfile::Strict};
  explicit FieldScratch(const Work& work) {
    wide.bind(work);
    small.bind(work);
  }
};
bool compact_number(double value) {
  return value == 0 ||
         (std::abs(value) >= 0x1p-100 && std::abs(value) <= 0x1p100);
}
struct DistanceField final : Program {
  FieldKind kind;
  ElementType type;
  double radius = 0, inner = 0, outer = 0;
  bool compact = false;
  DistanceField(FieldKind k, const std::vector<OperationMetadata>& in,
                const Parameters& p)
      : kind(k) {
    const auto& s = image(in[0], 2);
    type = s.descriptor.element_type;
    distance_description(p);
    if (kind == FieldKind::Shift || kind == FieldKind::Threshold) {
      radius = real(p, "radius");
    } else {
      inner = real(p, "inner");
      outer = real(p, "outer");
    }
    compact = compact_number(radius) && compact_number(inner) &&
              compact_number(outer);
    scratch_bytes = sizeof(FieldScratch);
    add_output(this, spatial_schema(type, s.sample_shape()));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, q}};
  }
  template <class Arithmetic>
  std::uint64_t exact_value(Arithmetic& e, double d, bool narrow) const {
    const auto v = e.binary(bits(d));
    if (kind == FieldKind::Shift) {
      return rounded(e, e.subtract(v, e.binary(bits(radius))), narrow);
    }
    const auto i = e.binary(bits(inner)), o = e.binary(bits(outer));
    const auto t = e.divide(e.add(v, i), e.add(i, o));
    return rounded(
        e, e.subtract(e.integer(1), curve01(e, t, kind == FieldKind::Smooth)),
        narrow);
  }
  template <class T>
  T evaluate(T value, FieldScratch& scratch) const {
    const bool narrow = sizeof(T) == 4;
    const double d = value;
    if (kind == FieldKind::Threshold) {
      return T(d <= radius);
    }
    if (kind == FieldKind::Shift) {
      if (radius == 0 || std::isinf(d)) {
        return value;
      }
      if (!narrow) {
        return T(d - radius);
      }
      if (static_cast<double>(static_cast<float>(radius)) == radius) {
        return value - static_cast<float>(radius);
      }
      const auto result = certified(Fast::point(d) - Fast::point(radius), true);
      if (result) {
        return T(numeric_ops::numeric_double(*result, true));
      }
    } else {
      if (inner == 0 && outer == 0) {
        return T(d <= 0);
      }
      if (d <= -inner) {
        return T(1);
      }
      if (d >= outer) {
        return T(0);
      }
      if (kind == FieldKind::Linear && inner == 1 && outer == 1) {
        return std::fma(T(-.5), value, T(.5));
      }
      const auto t = (Fast::point(d) + Fast::point(inner)) /
                     (Fast::point(inner) + Fast::point(outer));
      const auto result = certified(
          unit(Fast::point(1) - fast_curve(t, kind == FieldKind::Smooth)),
          narrow);
      if (result) {
        return T(numeric_ops::numeric_double(*result, narrow));
      }
    }
    const auto raw = compact && compact_number(d)
                         ? exact_value(scratch.small, d, narrow)
                         : exact_value(scratch.wide, d, narrow);
    return T(numeric_ops::numeric_double(raw, narrow));
  }
  template <class T>
  void run_t(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
             const Work& work, FieldScratch& scratch) const {
    const Reader reader{kernel, inv};
    unary_rows<T>(
        reader, inv.outputs[0], work,
        [&](const void* a, std::ptrdiff_t as, void* o, std::ptrdiff_t os,
            std::uint64_t count) {
          std::uint64_t j = 0;
#if defined(__aarch64__)
          const bool linear =
              kind == FieldKind::Linear && inner == 1 && outer == 1;
          const bool shift =
              kind == FieldKind::Shift &&
              (sizeof(T) == 8 ||
               static_cast<double>(static_cast<float>(radius)) == radius);
          if (as == sizeof(T) && os == sizeof(T) &&
              (linear || shift || kind == FieldKind::Threshold)) {
            if constexpr (sizeof(T) == 4) {
              auto threshold = static_cast<float>(radius);
              if (static_cast<double>(threshold) > radius)
                threshold = std::nextafter(threshold, -INFINITY);
              for (; j + 4 <= count; j += 4) {
                const auto v = vld1q_f32(static_cast<const float*>(a) + j);
                float32x4_t result;
                if (linear)
                  result = vminq_f32(
                      vdupq_n_f32(1),
                      vmaxq_f32(vdupq_n_f32(0),
                                vfmaq_n_f32(vdupq_n_f32(.5f), v, -.5f)));
                else if (shift)
                  result = radius == 0 ? v : vsubq_f32(v, vdupq_n_f32(radius));
                else
                  result = vreinterpretq_f32_u32(
                      vandq_u32(vcleq_f32(v, vdupq_n_f32(threshold)),
                                vreinterpretq_u32_f32(vdupq_n_f32(1))));
                vst1q_f32(static_cast<float*>(o) + j, result);
              }
            } else {
              for (; j + 2 <= count; j += 2) {
                const auto v = vld1q_f64(static_cast<const double*>(a) + j);
                float64x2_t result;
                if (linear)
                  result = vminq_f64(
                      vdupq_n_f64(1),
                      vmaxq_f64(vdupq_n_f64(0),
                                vfmaq_n_f64(vdupq_n_f64(.5), v, -.5)));
                else if (shift)
                  result = radius == 0 ? v : vsubq_f64(v, vdupq_n_f64(radius));
                else
                  result = vreinterpretq_f64_u64(
                      vandq_u64(vcleq_f64(v, vdupq_n_f64(radius)),
                                vreinterpretq_u64_f64(vdupq_n_f64(1))));
                vst1q_f64(static_cast<double*>(o) + j, result);
              }
            }
          }
#endif
          for (; j < count; ++j)
            save(o, os, j, evaluate(load<T>(a, as, j), scratch));
        });
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    auto* scratch = new (inv.scratch[0].allocation) FieldScratch(work);
    struct Destroy {
      FieldScratch* p;
      ~Destroy() { p->~FieldScratch(); }
    } destroy{scratch};
    if (type == ElementType::Float32) {
      run_t<float>(kernel, inv, work, *scratch);
    } else {
      run_t<double>(kernel, inv, work, *scratch);
    }
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_distance_fields(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry :
         {std::make_pair("shift_distance_field", FieldKind::Shift),
          std::make_pair("threshold_distance_field", FieldKind::Threshold),
          std::make_pair("distance_feather_linear", FieldKind::Linear),
          std::make_pair("distance_feather_smoothstep", FieldKind::Smooth)}) {
      std::vector<Parameter> p{{"distance_unit", Type::String},
                               {"distance_basis", Type::String}};
      if (entry.second == FieldKind::Shift ||
          entry.second == FieldKind::Threshold) {
        p.push_back({"radius", Type::Float64});
      } else {
        p.push_back({"inner", Type::Float64});
        p.push_back({"outer", Type::Float64});
      }
      check(registry->register_operation(definition(
          entry.first, 1, std::move(p),
          [kind = entry.second](const auto& in, const auto& parameters) {
            return std::make_shared<DistanceField>(kind, in, parameters);
          })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
