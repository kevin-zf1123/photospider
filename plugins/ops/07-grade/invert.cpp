// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "07-grade/invert_math.hpp"
#include "common/registry.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal::grade::invert {
using namespace generation;  // NOLINT(build/namespaces)

inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
template <class T, class V>
inline auto address(const V& v, const Coordinate& q) {
  using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                  const unsigned char, unsigned char>;
  using Pointer = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                     const T*, T*>;
  return reinterpret_cast<Pointer>(static_cast<Byte*>(v.allocation) +
                                   offset(*v.layout, v.byte_offset, q));
}
template <class T>
inline bool aligned(const T* p) {
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
}

/** Same-shape reflection with independent selected-channel bounds and bypass
 * support. Dynamic controls are read anew on every invocation.
 */
struct Invert final : Program {
  ElementType type;
  unsigned axis = 0;
  std::vector<std::uint64_t> dims, selected;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> rows;

  Invert(const std::vector<OperationMetadata>& input, const Parameters& p) {
    const auto& source = tensor(input.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD17 input requires Float32/64");
    if (p.count("input_description")) {
      require(!p.count("axis") && !p.count("components") && p.count("group"),
              "GRD17 description requires group without raw selectors");
      const auto description =
          take(tensor_description_from_parameter(text(p, "input_description")));
      check(validate_tensor_description(description, source.descriptor));
      require(description.channel_axis.has_value(),
              "GRD17 requires a component axis");
      const auto name = text(p, "group");
      const auto group =
          std::find_if(description.groups.begin(), description.groups.end(),
                       [&](const auto& g) { return g.name == name; });
      require(group != description.groups.end(), "GRD17 group not found");
      axis = *description.channel_axis + source.batch_axes.size();
      selected = group->indices;
    } else {
      require(!p.count("group") && p.count("axis") && p.count("components"),
              "GRD17 requires axis and components");
      const auto a = integer(p, "axis");
      require(a >= 0 && static_cast<std::uint64_t>(a) <
                            source.descriptor.shape.size(),
              "GRD17 axis outside tensor rank");
      axis = static_cast<unsigned>(a + source.batch_axes.size());
      selected = mask::channels(text(p, "components"), dims[axis]);
    }
    if (source.layout.spatial)
      require(
          source.layout.channel_axis &&
              *source.layout.channel_axis + source.batch_axes.size() == axis,
          "GRD17 selector must use spatial channel axis");
    require(!selected.empty(), "GRD17 requires selected components");
    const auto& bounds = tensor(input.at(1));
    require(bounds.descriptor.element_type == ElementType::Float64 &&
                bounds.sample_shape() ==
                    std::vector<std::uint64_t>{selected.size(), 2},
            "GRD17 bounds require Float64[Csel,2]");
    for (std::uint64_t i = 0; i < selected.size(); ++i)
      rows.emplace_back(selected[i], i);
    std::sort(rows.begin(), rows.end());
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
    std::vector<ReadRegion> reads{{0, 0, r}};
    const auto d = r.dimensions()[axis];
    for (unsigned c = 0; c < selected.size(); ++c)
      if (selected[c] >= d.offset && selected[c] - d.offset < d.extent)
        reads.push_back({1, 0, Region({{c, 1}, {0, 2}})});
    return reads;
  }

  Control control(const mask::Reader& reader, std::uint64_t channel) const {
    const auto it =
        std::lower_bound(rows.begin(), rows.end(), channel,
                         [](const auto& row, auto c) { return row.first < c; });
    if (it == rows.end() || it->first != channel)
      return {};
    Coordinate q{};
    q[0] = it->second;
    const auto lo = reader.value(1, 0, q);
    q[1] = 1;
    return {lo + reader.value(1, 0, q), true};
  }

  template <class T, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out) const {
    const auto& l = *out.layout;
    if (!axis || axis + 1 != l.rank || l.origin[axis] != 0 ||
        l.shape[axis] != C || dims[axis] != C || l.strides[axis] != sizeof(T) ||
        l.strides[axis - 1] != C * sizeof(T))
      return false;
    const unsigned run = axis - 1;
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a < run; ++a)
      row_count *= l.shape[a];
    Control controls[C];
    for (unsigned c = 0; c < C; ++c)
      controls[c] = control(reader, c);
    // Preflight retained spans before writing; component-split tiles use lines.
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a)
        at[a] = l.origin[a];
      for (std::uint64_t row = 0; row < row_count; ++row) {
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          const auto& in = reader.view(0, 0, at);
          const auto& il = *in.layout;
          const auto* x = address<T>(in, at);
          auto* y = address<T>(out, at);
          if (il.origin[axis] != 0 || il.shape[axis] != C ||
              il.strides[axis] != sizeof(T) ||
              il.strides[run] != C * sizeof(T) || !aligned(x) || !aligned(y))
            return false;
          const auto n = std::min<std::uint64_t>(
              {8192 / C, l.origin[run] + l.shape[run] - at[run],
               il.origin[run] + il.shape[run] - at[run]});
          if (pass)
            span<T, C>(x, y, n, controls,
                       row_count * l.shape[run] * C * sizeof(T) >= 1024 * 1024);
          at[run] += n;
        }
        for (unsigned a = run; a-- > 0;) {
          if (++at[a] < l.origin[a] + l.shape[a])
            break;
          at[a] = l.origin[a];
        }
      }
    }
    return true;
  }

  template <class T>
  void lines(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
             const ps_kernel_mutable_view_v8& out) const {
    const auto& l = *out.layout;
    if (l.rank == 1) {
      samples(out, [&](const Coordinate& at, std::uint64_t index) {
        if ((index & 255U) == 0)
          cancel(inv);
        const auto raw = reader.raw(0, 0, at);
        const auto c = control(reader, at[axis]);
        if (!c.active) {
          store(out, at, raw);
        } else {
          T x, y;
          std::memcpy(&x, &raw, sizeof(T));
          scalar(&x, &y, c);
          std::uint64_t result = 0;
          std::memcpy(&result, &y, sizeof(T));
          store(out, at, result);
        }
      });
      return;
    }
    const unsigned run = l.rank - 1 == axis ? l.rank - 2 : l.rank - 1;
    Coordinate at{};
    std::uint64_t row_count = 1;
    for (unsigned a = 0; a < l.rank; ++a) {
      at[a] = l.origin[a];
      if (a != run)
        row_count *= l.shape[a];
    }
    for (std::uint64_t row = 0; row < row_count; ++row) {
      const auto c = control(reader, at[axis]);
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        cancel(inv);
        const auto& in = reader.view(0, 0, at);
        const auto& il = *in.layout;
        const auto n = std::min<std::uint64_t>(
            {8192, l.origin[run] + l.shape[run] - at[run],
             il.origin[run] + il.shape[run] - at[run]});
        const auto* x = address<T>(in, at);
        auto* y = address<T>(out, at);
        if (il.strides[run] == sizeof(T) && l.strides[run] == sizeof(T) &&
            aligned(x) && aligned(y)) {
          plane(x, y, n, c,
                row_count * l.shape[run] * sizeof(T) >= 1024 * 1024);
        } else {
          for (std::uint64_t j = 0; j < n; ++j) {
            // memcpy handles byte-unaligned and negative-stride views.
            const auto v = mask::load<T>(x, il.strides[run], j);
            T result;
            scalar(&v, &result, c);
            mask::save(y, l.strides[run], j, result);
          }
        }
        at[run] += n;
      }
      for (unsigned a = l.rank; a-- > 0;) {
        if (a == run)
          continue;
        if (++at[a] < l.origin[a] + l.shape[a])
          break;
        at[a] = l.origin[a];
      }
    }
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned output = 0; output < inv.output_count; ++output) {
      const auto& out = inv.outputs[output];
      const auto& l = *out.layout;
      std::uint64_t count = 1, requested = 0;
      for (unsigned a = 0; a < l.rank; ++a)
        count *= l.shape[a];
      if (!count)
        continue;
      for (auto c : selected)
        requested += c >= l.origin[axis] && c - l.origin[axis] < l.shape[axis];
      // Descriptor-derived admission precedes all payload reads and writes.
      work.poll(count + count / l.shape[axis] * requested + 2 * requested);
      if (dims[axis] == 1 && packed<T, 1>(reader, inv, out))
        continue;
      if (dims[axis] == 3 && packed<T, 3>(reader, inv, out))
        continue;
      if (dims[axis] == 4 && packed<T, 4>(reader, inv, out))
        continue;
      lines<T>(reader, inv, out);
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

OperationDefinition operation() {
  using Type = OperationParameterType;
  auto definition = budgeted_operation(generation::operation(
      "grade.invert", 2,
      {{"axis", Type::Int64, false},
       {"components", Type::String, false},
       {"input_description", Type::String, false},
       {"group", Type::String, false}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [](const auto& inputs, const auto& parameters) {
        return std::make_shared<Invert>(inputs, parameters);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade::invert

namespace ps::plugin_internal {
Status register_grade_invert(OperationRegistry* registry) {
  return registry->register_operation(grade::invert::operation());
}
}  // namespace ps::plugin_internal
