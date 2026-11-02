// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/threshold_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::threshold {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty_region(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return d.extent == 0; });
}
inline void cancellation(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
/** A collective local filter uses a full-image numerical grid. Output requests
 * select writes only; the arithmetic order never depends on the output tiles.
 * Scratch is declared before execution and no dynamic image is retained. */
struct Adjustment final : Program {
  Filter filter;
  ElementType type;
  unsigned axis = 0;
  bool keepdims = true;
  std::vector<std::uint64_t> dims;
  Adjustment(unsigned kind, const std::vector<OperationMetadata>& input,
             const Parameters& p) {
    filter.kind = kind;
    const auto& source = tensor(input.at(0), kind == 0);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD14 requires Float32/64");
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    auto& t = output.tensors[0];
    t.key = "samples";
    t.descriptor.element_type = ElementType::UInt8;
    t.atomic_trailing_axes = 0;
    if (kind == 0) {
      try {
        const auto selection = color_point::select(input[0], p);
        require(selection.axis >= 0 && selection.channels.size() == 1,
                "GRD14A requires one feature component");
        axis = static_cast<unsigned>(selection.axis);
        require(dims[axis] == 1 && selection.channels[0] == 0,
                "GRD14A feature extent must be one");
      } catch (const basis_ops::CheckedFailure& e) {
        throw e.status;
      }
      keepdims = color_point::boolean(p, "keepdims", true);
      if (!keepdims) {
        require(t.descriptor.shape.size() > 1,
                "GRD14A cannot remove the last descriptor axis");
        const auto a = axis - source.batch_axes.size();
        t.descriptor.shape.erase(t.descriptor.shape.begin() + a);
        t.layout.channel_axis.reset();
        if (t.layout.spatial) {
          if (t.layout.height_axis > a)
            --t.layout.height_axis;
          if (t.layout.width_axis > a)
            --t.layout.width_axis;
        }
      }
    } else {
      require(source.descriptor.shape.size() == 2,
              "GRD14 local filters require H,W");
      mask::spatial_axes(source, 2);
      filter.height = dims[0];
      filter.width = dims[1];
      const auto radius = integer(p, "radius", kind == 3 ? 7 : 3);
      require(radius >= 0 && radius <= 4096, "GRD14 radius outside 0..4096");
      filter.radius = static_cast<std::size_t>(radius);
      filter.sigma = real(p, "sigma", 2);
      filter.k = real(p, "k", .2);
      filter.dynamic_range = real(p, "dynamic_range", .5);
      filter.prepare();
      collective = filter.radius != 0;
      if (collective)
        scratch_bytes =
            mask::bytes(static_cast<unsigned __int128>(filter.scratch_count()) *
                        sizeof(double));
    }
    if (kind != 3) {
      const auto& control = tensor(input.at(1));
      require(control.descriptor.element_type == ElementType::Float64 &&
                  control.sample_shape() == std::vector<std::uint64_t>{1},
              "GRD14 control requires Float64[1]");
    }
    check(output.validate());
    add_output(this, std::move(output));
  }
  Region source_region(const Region& r) const {
    if (filter.kind != 0 || keepdims)
      return r;
    auto d = r.dimensions();
    d.insert(d.begin() + axis, {0, 1});
    return Region(std::move(d));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (!collective && empty_region(r))
      return {};
    std::vector<ReadRegion> reads{
        {0, 0, collective ? Region::whole(dims) : source_region(r)}};
    if (filter.kind != 3)
      reads.push_back({1, 0, Region({{0, 1}})});
    return reads;
  }
  Coordinate source_position(const Coordinate& at, unsigned output_rank) const {
    if (filter.kind != 0 || keepdims)
      return at;
    Coordinate q{};
    for (unsigned a = 0, b = 0; a < output_rank + 1; ++a)
      q[a] = a == axis ? 0 : at[b++];
    return q;
  }
  template <class T>
  void gather(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              double* source, const Work& work) const {
    Coordinate at{};
    for (at[0] = 0; at[0] < filter.height; ++at[0]) {
      for (at[1] = 0; at[1] < filter.width;) {
        const auto& v = reader.view(0, 0, at);
        const auto n = std::min<std::uint64_t>(
            {4096, filter.width - at[1],
             v.layout->origin[1] + v.layout->shape[1] - at[1]});
        work.poll(n);
        const auto* ptr = static_cast<const unsigned char*>(v.allocation) +
                          offset(*v.layout, v.byte_offset, at);
        auto* dst = source + at[0] * filter.width + at[1];
        if (v.layout->strides[1] == sizeof(T) &&
            reinterpret_cast<std::uintptr_t>(ptr) % alignof(T) == 0) {
          const auto* values = reinterpret_cast<const T*>(ptr);
          for (std::uint64_t j = 0; j < n; ++j)
            dst[j] = values[j];
        } else {
          for (std::uint64_t j = 0; j < n; ++j)
            dst[j] = mask::load<T>(ptr, v.layout->strides[1], j);
        }
        at[1] += n;
      }
    }
    cancellation(inv);
  }
  template <class T>
  bool packed_fixed(const mask::Reader& reader,
                    const ps_kernel_invocation_v8& inv,
                    const ps_kernel_mutable_view_v8& out, std::uint64_t count,
                    double control) const {
    if (filter.kind != 0)
      return false;
    Coordinate at{};
    for (unsigned a = 0; a < out.layout->rank; ++a)
      at[a] = out.layout->origin[a];
    const auto q = source_position(at, out.layout->rank);
    const auto& view = reader.view(0, 0, q);
    const auto& l = *view.layout;
    std::uint64_t stride = sizeof(T);
    for (unsigned a = l.rank; a-- > 0;) {
      const unsigned b = (!keepdims && a > axis) ? a - 1 : a;
      const auto extent = (!keepdims && a == axis) ? 1 : out.layout->shape[b];
      if (l.origin[a] != q[a] || l.shape[a] != extent ||
          (extent > 1 && l.strides[a] != static_cast<std::int64_t>(stride)))
        return false;
      stride *= extent;
    }
    const auto* source =
        static_cast<const unsigned char*>(view.allocation) + view.byte_offset;
    if (reinterpret_cast<std::uintptr_t>(source) % alignof(T))
      return false;
    auto* target = static_cast<std::uint8_t*>(out.allocation) + out.byte_offset;
    for (std::uint64_t i = 0; i < count; i += 4096) {
      cancellation(inv);
      fixed_span(reinterpret_cast<const T*>(source) + i, target + i,
                 std::min<std::uint64_t>(4096, count - i), control);
    }
    return true;
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    if (!inv.output_count)
      return;
    const mask::Reader reader{kernel, inv};
    const double control =
        filter.kind == 3 ? 0 : reader.value(1, 0, Coordinate{});
    double* source = nullptr;
    double* scratch = nullptr;
    if (collective) {
      source = static_cast<double*>(inv.scratch[0].allocation);
      scratch = source + filter.height * filter.width;
      gather<T>(reader, inv, source, work);
      filter.build(source, scratch, [&](auto amount) { work.poll(amount); });
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      std::uint64_t count = 1;
      for (unsigned a = 0; a < out.layout->rank; ++a)
        count *= out.layout->shape[a];
      if (!count)
        continue;
      const auto cost = filter.kind == 2 ? 8 * (2 * filter.radius + 1) : 64;
      work.poll(count * cost);
      if (packed_fixed<T>(reader, inv, out, count, control))
        continue;
      if (collective) {
        const auto& l = *out.layout;
        for (std::uint64_t y = l.origin[0]; y < l.origin[0] + l.shape[0]; ++y) {
          cancellation(inv);
          Coordinate at{};
          at[0] = y;
          at[1] = l.origin[1];
          auto* target = static_cast<std::uint8_t*>(out.allocation) +
                         offset(l, out.byte_offset, at);
          filter.row(source, scratch, y, l.origin[1], l.shape[1], control,
                     target, [&](auto amount) { work.poll(amount); });
        }
        continue;
      }
      samples(out, [&](const Coordinate& at, std::uint64_t i) {
        if ((i & 1023U) == 0)
          cancellation(inv);
        bool value;
        if (collective) {
          value = filter.value(source, scratch, at[0], at[1], control,
                               [&](auto amount) { work.poll(amount); });
        } else {
          const auto q = source_position(at, out.layout->rank);
          const double x = reader.value(0, 0, q);
          value = filter.kind == 0   ? x >= control
                  : filter.kind == 3 ? x >= x * (1 - filter.k)
                                     : x >= x - control;
        }
        store(out, at, value ? 1 : 0);
      });
    }
    cancellation(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv, Work{inv, work_limit});
    else
      compute<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
    else
      compute<double>(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
};
OperationDefinition operation(unsigned kind) {
  using Type = OperationParameterType;
  static constexpr const char* names[] = {
      "grade.threshold", "grade.threshold_local_mean",
      "grade.threshold_local_gaussian", "grade.threshold_sauvola"};
  std::vector<OperationParameterSpec> p;
  if (kind == 0) {
    p = {{"axis", Type::Int64, false},
         {"components", Type::String, false},
         {"input_description", Type::String, false},
         {"group", Type::String, false},
         {"keepdims", Type::Bool, false}};
  } else {
    p.push_back({"radius", Type::Int64, false});
    if (kind == 2)
      p.push_back({"sigma", Type::Float64, false});
    if (kind == 3) {
      p.push_back({"k", Type::Float64, false});
      p.push_back({"dynamic_range", Type::Float64, false});
    }
  }
  return budgeted_operation(generation::operation(
      names[kind], kind == 3 ? 1 : 2, std::move(p),
      {{"mask", tensor_schema(ElementType::UInt8, {1})}},
      [kind](const auto& inputs, const auto& params) {
        return std::make_shared<Adjustment>(kind, inputs, params);
      }));
}
}  // namespace ps::plugin_internal::grade::threshold
namespace ps::plugin_internal {
Status register_grade_threshold(OperationRegistry* registry) {
  for (unsigned kind = 0; kind < 4; ++kind) {
    const auto status =
        registry->register_operation(grade::threshold::operation(kind));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
