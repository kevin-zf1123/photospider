// Copyright 2026 Photospider contributors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "09-composite/blend_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::blend {
using namespace generation;  // NOLINT(build/namespaces)

inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}

/** Number of jointly dense samples beginning at a logical output coordinate.
 * Extending across an outer dimension requires the input to contain the whole
 * inner output extent. A cropped row therefore never crosses an input gap.
 */
inline std::uint64_t dense(const ps_kernel_layout_v8& out,
                           const ps_kernel_layout_v8& in, const Coordinate& at,
                           std::uint64_t width) {
  std::uint64_t n = 1, inner = 1;
  for (unsigned a = out.rank; a-- > 0;) {
    if ((out.shape[a] > 1 &&
         out.strides[a] != static_cast<std::int64_t>(width * inner)) ||
        (in.shape[a] > 1 &&
         in.strides[a] != static_cast<std::int64_t>(width * inner)))
      break;
    const auto end =
        std::min(out.origin[a] + out.shape[a], in.origin[a] + in.shape[a]);
    n += (end - at[a] - 1) * inner;
    if (in.origin[a] > out.origin[a] ||
        in.origin[a] + in.shape[a] < out.origin[a] + out.shape[a])
      break;
    inner *= out.shape[a];
  }
  return n;
}
inline void advance(Coordinate* at, const ps_kernel_layout_v8& layout,
                    std::uint64_t n) {
  for (unsigned a = layout.rank; a-- > 0 && n;) {
    n += (*at)[a] - layout.origin[a];
    (*at)[a] = layout.origin[a] + n % layout.shape[a];
    n /= layout.shape[a];
  }
}

template <Kind K>
struct Blend final : Program {
  ElementType type;

  Blend(const std::vector<OperationMetadata>& input, const Parameters& p) {
    const auto& background = tensor(input.at(0), true);
    const auto& source = tensor(input.at(1), true);
    type = background.descriptor.element_type;
    require(floating(type) && source.descriptor.element_type == type &&
                background.descriptor.shape == source.descriptor.shape &&
                background.sample_shape() == source.sample_shape() &&
                background.batch_axes.size() == source.batch_axes.size(),
            "BLN inputs require matching Float32/64 color tensors");
    try {
      const auto b = color_point::select(input[0], p);
      const auto s = color_point::select(input[1], p);
      require(b.axis >= 0 && b.axis == s.axis && b.channels == s.channels,
              "BLN requires an explicit matching color axis");
      const auto k = b.channels.size();
      require((k == 1 || k == 3) && background.sample_shape()[b.axis] == k,
              "BLN color-only axis must contain exactly Gray or RGB");
      require(
          !b.semantic || b.interpretation.model == (k == 1 ? "gray" : "rgb"),
          "BLN description requires Gray or RGB coordinates");
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    }
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    check(output.validate());
    add_output(this, std::move(output));
  }

  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    if constexpr (K == Kind::Normal)
      return {{1, 0, r}};
    return {{0, 0, r}, {1, 0, r}};
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned output = 0; output < inv.output_count; ++output) {
      const auto& out = inv.outputs[output];
      const auto& layout = *out.layout;
      Coordinate at{};
      std::uint64_t count = 1;
      for (unsigned a = 0; a < layout.rank; ++a) {
        at[a] = layout.origin[a];
        count *= layout.shape[a];
      }
      if (!count)
        continue;
      constexpr std::uint64_t kWork =
          K == Kind::Normal       ? 1
          : K == Kind::SoftLight  ? 22
          : K == Kind::VividLight ? 20
          : (K == Kind::PinLight || K == Kind::HardMixVivid ||
             K == Kind::HardMixSum)
              ? 8
          : K == Kind::LinearLight                            ? 6
          : K == Kind::Exclusion                              ? 5
          : (K == Kind::ColorDodge || K == Kind::ColorBurn)   ? 10
          : (K == Kind::Overlay || K == Kind::HardLight)      ? 9
          : (K == Kind::Screen || K == Kind::ScreenUnbounded) ? 5
                                                              : 3;
      // Full descriptor-derived admission precedes this tile's payload access.
      work.poll(count * kWork);
      for (std::uint64_t done = 0; done < count;) {
        cancel(inv);
        const auto& source = reader.view(1, 0, at);
        const ps_kernel_const_view_v8* background = nullptr;
        if constexpr (K != Kind::Normal)
          background = &reader.view(0, 0, at);
        const auto* s = static_cast<const unsigned char*>(source.allocation) +
                        offset(*source.layout, source.byte_offset, at);
        const auto* b =
            background
                ? static_cast<const unsigned char*>(background->allocation) +
                      offset(*background->layout, background->byte_offset, at)
                : nullptr;
        auto* target = static_cast<unsigned char*>(out.allocation) +
                       offset(layout, out.byte_offset, at);
        auto n = std::min<std::uint64_t>(
            {16384, count - done,
             dense(layout, *source.layout, at, sizeof(T))});
        if constexpr (K != Kind::Normal)
          n = std::min(n, dense(layout, *background->layout, at, sizeof(T)));
        const bool aligned =
            reinterpret_cast<std::uintptr_t>(s) % alignof(T) == 0 &&
            reinterpret_cast<std::uintptr_t>(target) % alignof(T) == 0 &&
            (!background ||
             reinterpret_cast<std::uintptr_t>(b) % alignof(T) == 0);
        if (n > 1 && aligned) {
          span<K>(reinterpret_cast<const T*>(b), reinterpret_cast<const T*>(s),
                  reinterpret_cast<T*>(target), n, count * sizeof(T));
        } else {
          // A byte-unaligned or signed-stride row retains the same arithmetic.
          const unsigned a = layout.rank - 1;
          n = std::min<std::uint64_t>(
              {16384, layout.origin[a] + layout.shape[a] - at[a],
               source.layout->origin[a] + source.layout->shape[a] - at[a]});
          if constexpr (K != Kind::Normal)
            n = std::min(n, background->layout->origin[a] +
                                background->layout->shape[a] - at[a]);
          for (std::uint64_t j = 0; j < n; ++j) {
            T s_value, b_value{};
            std::memcpy(
                &s_value,
                s + static_cast<std::ptrdiff_t>(j) * source.layout->strides[a],
                sizeof(T));
            if constexpr (K != Kind::Normal)
              std::memcpy(&b_value,
                          b + static_cast<std::ptrdiff_t>(j) *
                                  background->layout->strides[a],
                          sizeof(T));
            const T value = evaluate<K, Scalar<T>>(b_value, s_value);
            std::memcpy(
                target + static_cast<std::ptrdiff_t>(j) * layout.strides[a],
                &value, sizeof(T));
          }
        }
        done += n;
        advance(&at, layout, n);
      }
    }
    cancel(inv);
  }

  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(kernel, inv, Work{inv, work_limit});
    else
      compute<double>(kernel, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& kernel,
              const ps_kernel_invocation_v9& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(kernel, inv.views,
                     Work{inv.views, work_limit, 0, inv.work});
    else
      compute<double>(kernel, inv.views,
                      Work{inv.views, work_limit, 0, inv.work});
  }
};

template <Kind K>
OperationDefinition operation(const char* key) {
  using P = OperationParameterType;
  return budgeted_operation(generation::operation(
      key, 2,
      {{"axis", P::Int64, false},
       {"components", P::String, false},
       {"input_description", P::String, false},
       {"group", P::String, false}},
      {{"color", tensor_schema(ElementType::Float64, {1})}},
      [](const auto& inputs, const auto& parameters) {
        return std::make_shared<Blend<K>>(inputs, parameters);
      }));
}
}  // namespace ps::plugin_internal::blend

namespace ps::plugin_internal {
Status register_blend(OperationRegistry* registry) {
  using blend::Kind;
  using blend::operation;
  for (auto definition :
       {operation<Kind::Normal>("blend.normal"),
        operation<Kind::Add>("blend.add"),
        operation<Kind::Subtract>("blend.subtract"),
        operation<Kind::Divide>("blend.divide"),
        operation<Kind::Multiply>("blend.multiply"),
        operation<Kind::MultiplyUnbounded>("blend.multiply_unbounded")}) {
    const auto status = registry->register_operation(std::move(definition));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
