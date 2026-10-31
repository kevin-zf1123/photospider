// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/contrast_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::contrast {
using namespace generation;  // NOLINT(build/namespaces)
constexpr std::size_t kBlock = 256;
constexpr std::uint64_t kScratch = 8 * kBlock * sizeof(double);

struct Adjustment final : Program {
  bool logarithmic;
  ElementType type;
  unsigned axis;
  std::vector<std::uint64_t> dims, selected;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> rows;

  Adjustment(bool contrast_log2, const std::vector<OperationMetadata>& input,
             const Parameters& parameters)
      : logarithmic(contrast_log2) {
    const auto& source = tensor(input.at(0), true);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    require(floating(type), "GRD03 image requires Float32/64");
    try {
      if (parameters.count("input_description")) {
        const auto selection = color_point::select(input[0], parameters);
        color_point::require(selection.axis >= 0,
                             "GRD03 requires a component axis");
        axis = static_cast<unsigned>(selection.axis);
        selected = selection.channels;
        if (logarithmic) {
          const auto& color = selection.interpretation;
          color_point::require(color.model == "rgb" || color.model == "gray",
                               "GRD03 log2 requires RGB or Gray");
          color_point::require(color.transfer == "linear",
                               "GRD03 log2 requires linear coordinates");
          if (color.model == "gray")
            color_point::require(
                color.coordinates && color.coordinates->gray_kind == "linear_y",
                "GRD03 log2 requires linear-Y Gray");
        }
      } else {
        color_point::require(!parameters.count("group") &&
                                 parameters.count("axis") &&
                                 parameters.count("components"),
                             "GRD03 requires axis and components");
        const auto a = integer(parameters, "axis");
        color_point::require(a >= 0 && static_cast<std::uint64_t>(a) <
                                           source.descriptor.shape.size(),
                             "GRD03 axis outside tensor rank");
        axis = static_cast<unsigned>(a + source.batch_axes.size());
        selected = mask::channels(text(parameters, "components"), dims[axis]);
        if (source.layout.spatial)
          color_point::require(
              source.layout.channel_axis == std::optional<std::uint32_t>(a),
              "GRD03 selector must use spatial channel axis");
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    }
    require(!selected.empty(), "GRD03 requires selected components");
    const auto& controls = tensor(input.at(1));
    require(controls.descriptor.element_type == ElementType::Float64 &&
                controls.sample_shape() ==
                    std::vector<std::uint64_t>{selected.size(), 2U},
            "GRD03 controls require Float64[Csel,2]");
    for (std::uint64_t i = 0; i < selected.size(); ++i)
      rows.emplace_back(selected[i], i);
    std::sort(rows.begin(), rows.end());
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    add_output(this, std::move(output));
    scratch_bytes = kScratch;
  }

  bool calculates(const Region& region) const {
    if (region.dimensions().empty())
      return false;
    for (const auto& d : region.dimensions())
      if (!d.extent)
        return false;
    const auto& c = region.dimensions()[axis];
    return std::any_of(selected.begin(), selected.end(), [&](auto s) {
      return s >= c.offset && s - c.offset < c.extent;
    });
  }
  std::uint64_t task_scratch(unsigned, const Region& region) const override {
    return calculates(region) ? kScratch : 0;
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (region.dimensions().empty())
      return {};
    for (const auto& d : region.dimensions())
      if (!d.extent)
        return {};
    std::vector<ReadRegion> reads{{0, 0, region}};
    if (calculates(region))
      reads.push_back({1, 0, Region({{0, selected.size()}, {0, 2U}})});
    return reads;
  }

  Control control(const mask::Reader& reader, std::uint64_t component) const {
    const auto found =
        std::lower_bound(rows.begin(), rows.end(), component,
                         [](const auto& row, auto c) { return row.first < c; });
    if (found == rows.end() || found->first != component)
      return {};
    Coordinate at{};
    at[0] = found->second;
    const double a = reader.value(1, 0, at);
    at[1] = 1;
    const double b = reader.value(1, 0, at);
    return make_control(a, b, logarithmic);
  }
  template <class T, class V>
  static auto address(const V& view, const Coordinate& at) {
    using Pointer =
        std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>, const T*,
                           T*>;
    using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                    const unsigned char, unsigned char>;
    return reinterpret_cast<Pointer>(
        static_cast<Byte*>(view.allocation) +
        generation::offset(*view.layout, view.byte_offset, at));
  }
  template <class T>
  static bool aligned(const T* p) {
    return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
  }
  static void cancel(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context))
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
  template <class T, unsigned C>
  void transform(const T* source, T* target, std::size_t n,
                 const Control* controls) const {
    if (logarithmic)
      span<T, C, true>(source, target, n, controls);
    else
      span<T, C, false>(source, target, n, controls);
  }

  template <class T, unsigned C>
  void pixels(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& output) const {
    const mask::Reader reader{kernel, inv};
    Control controls[C];
    for (unsigned c = 0; c < C; ++c)
      controls[c] = control(reader, c);
    const unsigned run_axis = axis - 1;
    Coordinate at{};
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a < axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        row_count *= output.layout->shape[a];
    }
    auto* sb = static_cast<T*>(inv.scratch[0].allocation);
    auto* db = sb + 4 * kBlock;
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < row_count; ++row) {
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        const auto n = std::min<std::uint64_t>(
            {kBlock, end - at[run_axis],
             input.layout->origin[run_axis] + input.layout->shape[run_axis] -
                 at[run_axis]});
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const bool direct = input.layout->origin[axis] == 0 &&
                            input.layout->shape[axis] == C &&
                            input.layout->strides[axis] == sizeof(T) &&
                            input.layout->strides[run_axis] == C * sizeof(T) &&
                            output.layout->strides[axis] == sizeof(T) &&
                            output.layout->strides[run_axis] == C * sizeof(T) &&
                            aligned(src) && aligned(dst);
        if (direct) {
          transform<T, C>(src, dst, n, controls);
        } else {
          auto q = at;
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              const auto raw = reader.raw(0, 0, q);
              std::memcpy(sb + C * j + c, &raw, sizeof(T));
            }
          }
          transform<T, C>(sb, db, n, controls);
          for (std::uint64_t j = 0; j < n; ++j) {
            q[run_axis] = at[run_axis] + j;
            for (unsigned c = 0; c < C; ++c) {
              q[axis] = c;
              std::uint64_t raw = 0;
              std::memcpy(&raw, db + C * j + c, sizeof(T));
              store(output, q, raw);
            }
          }
        }
        at[run_axis] += n;
      }
      for (unsigned a = run_axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
  }

  template <class T>
  void lines(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
             const ps_kernel_mutable_view_v8& output, bool arithmetic) const {
    const mask::Reader reader{kernel, inv};
    const unsigned run_axis = output.layout->rank - 1;
    if (run_axis == axis) {
      std::uint64_t pending = 0;
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        if ((pending++ & 127U) == 0)
          cancel(inv);
        const auto raw = reader.raw(0, 0, at);
        const auto c = control(reader, at[axis]);
        if (c.copy) {
          store(output, at, raw);
          return;
        }
        T x;
        std::memcpy(&x, &raw, sizeof(T));
        store(
            output, at,
            convert(logarithmic ? evaluate<true>(x, c) : evaluate<false>(x, c),
                    sizeof(T) == 4));
      });
      return;
    }
    Coordinate at{};
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a <= run_axis; ++a) {
      at[a] = output.layout->origin[a];
      if (a != run_axis)
        row_count *= output.layout->shape[a];
    }
    auto* sb =
        arithmetic ? static_cast<T*>(inv.scratch[0].allocation) : nullptr;
    auto* db = arithmetic ? sb + 4 * kBlock : nullptr;
    const auto end =
        output.layout->origin[run_axis] + output.layout->shape[run_axis];
    for (std::uint64_t row = 0; row < row_count; ++row) {
      const auto c = control(reader, at[axis]);
      for (at[run_axis] = output.layout->origin[run_axis];
           at[run_axis] < end;) {
        cancel(inv);
        const auto& input = reader.view(0, 0, at);
        const auto n = std::min<std::uint64_t>(
            {kBlock, end - at[run_axis],
             input.layout->origin[run_axis] + input.layout->shape[run_axis] -
                 at[run_axis]});
        const auto* src = address<T>(input, at);
        auto* dst = address<T>(output, at);
        const auto is = input.layout->strides[run_axis];
        const auto os = output.layout->strides[run_axis];
        if (c.copy) {
          if (is == sizeof(T) && os == sizeof(T)) {
            std::memcpy(dst, src, n * sizeof(T));
          } else {
            for (std::uint64_t j = 0; j < n; ++j)
              std::memcpy(reinterpret_cast<unsigned char*>(dst) +
                              static_cast<std::ptrdiff_t>(j) * os,
                          reinterpret_cast<const unsigned char*>(src) +
                              static_cast<std::ptrdiff_t>(j) * is,
                          sizeof(T));
          }
        } else if (is == sizeof(T) && os == sizeof(T) && aligned(src) &&
                   aligned(dst)) {
          transform<T, 1>(src, dst, n, &c);
        } else {
          for (std::uint64_t j = 0; j < n; ++j)
            sb[j] = mask::load<T>(src, is, j);
          transform<T, 1>(sb, db, n, &c);
          for (std::uint64_t j = 0; j < n; ++j)
            mask::save(dst, os, j, db[j]);
        }
        at[run_axis] += n;
      }
      for (unsigned a = run_axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      std::uint64_t count = 1;
      for (unsigned a = 0; a < output.layout->rank; ++a)
        count *= output.layout->shape[a];
      if (!count)
        continue;
      const bool arithmetic =
          std::any_of(selected.begin(), selected.end(), [&](auto c) {
            return c >= output.layout->origin[axis] &&
                   c - output.layout->origin[axis] < output.layout->shape[axis];
          });
      // Charge the descriptor-derived upper bound before reading or writing.
      work.poll(count * (logarithmic ? 128 : 8));
      const bool interleaved = arithmetic && axis > 0 &&
                               axis + 1 == dims.size() &&
                               output.layout->origin[axis] == 0 &&
                               output.layout->shape[axis] == dims[axis];
      if (interleaved && dims[axis] == 1)
        pixels<T, 1>(kernel, inv, output);
      else if (interleaved && dims[axis] == 3)
        pixels<T, 3>(kernel, inv, output);
      else if (interleaved && dims[axis] == 4)
        pixels<T, 4>(kernel, inv, output);
      else
        lines<T>(kernel, inv, output, arithmetic);
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

OperationDefinition operation(bool logarithmic) {
  using Type = OperationParameterType;
  return budgeted_operation(generation::operation(
      logarithmic ? "grade.contrast_log2" : "grade.contrast_linear", 2,
      {{"axis", Type::Int64, false},
       {"components", Type::String, false},
       {"input_description", Type::String, false},
       {"group", Type::String, false}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [logarithmic](const auto& inputs, const auto& parameters) {
        return std::make_shared<Adjustment>(logarithmic, inputs, parameters);
      }));
}
}  // namespace ps::plugin_internal::grade::contrast
namespace ps::plugin_internal {
Status register_grade_contrast(OperationRegistry* registry) {
  for (bool logarithmic : {false, true}) {
    const auto status =
        registry->register_operation(grade::contrast::operation(logarithmic));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
