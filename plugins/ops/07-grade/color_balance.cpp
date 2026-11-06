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
#include "07-grade/color_balance_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::balance {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context)) {
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
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
/** Same-shape tonal balance. Only requested adjusted coordinates consume zones
 * and corresponding offset columns; lightness, alpha and AOVs are independent.
 */
struct Balance final : Program {
  unsigned axis = 0, components = 3;
  ElementType type, zone_type;
  std::vector<std::uint64_t> dims, selected;

  Balance(bool opponent, const std::vector<OperationMetadata>& input,
          const Parameters& p) {
    const auto& source = tensor(input.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    try {
      const auto s = color_point::select(
          input[0], p,
          opponent ? std::vector<std::string>{"l", "a", "b"}
                   : std::vector<std::string>{"red", "green", "blue"});
      require(s.axis >= 0 && s.channels.size() == 3,
              "GRD16 requires three selected coordinates");
      axis = static_cast<unsigned>(s.axis);
      selected = s.channels;
      const auto model =
          text(p, "model", s.semantic ? s.interpretation.model.c_str() : "");
      if (opponent) {
        require(model == "cielab" || model == "oklab",
                "GRD16B requires model=cielab|oklab or a native description");
        require(!s.semantic || model == s.interpretation.model,
                "GRD16B model disagrees with description");
        for (unsigned c = 0; c < 3; ++c) {
          const auto& u = s.units[c];
          const bool cie = model == "cielab" &&
                           ((c == 1 && u == "a*") || (c == 2 && u == "b*"));
          require(u.empty() || u == "1" || u == "relative" || cie,
                  "GRD16B requires native coordinate units");
        }
        selected.erase(selected.begin());
      } else {
        require(!s.semantic || s.interpretation.model == "rgb",
                "GRD16A requires RGB coordinates");
      }
    } catch (const basis_ops::CheckedFailure& failure) {
      throw failure.status;
    }
    components = static_cast<unsigned>(selected.size());
    const auto& zones = tensor(input.at(1), true);
    auto expected = dims;
    expected[axis] = 3;
    zone_type = zones.descriptor.element_type;
    require(floating(zone_type) && zones.sample_shape() == expected,
            "GRD16 zones require Float32/64 with the same sample axes and "
            "three zones");
    const auto& offsets = tensor(input.at(2));
    require(
        offsets.descriptor.element_type == ElementType::Float64 &&
            offsets.sample_shape() == std::vector<std::uint64_t>{3, components},
        "GRD16 offsets require Float64[3,3] or Float64[3,2]");
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    check(output.validate());
    add_output(this, std::move(output));
  }
  bool includes(const Region& r, std::uint64_t c) const {
    const auto d = r.dimensions()[axis];
    return c >= d.offset && c - d.offset < d.extent;
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r))
      return {};
    std::vector<ReadRegion> reads{{0, 0, r}};
    bool active = false;
    for (unsigned c = 0; c < components; ++c) {
      if (includes(r, selected[c])) {
        active = true;
        reads.push_back({2, 0, Region({{0, 3}, {c, 1}})});
      }
    }
    if (active) {
      auto zones = r.dimensions();
      zones[axis] = {0, 3};
      reads.push_back({1, 0, Region(std::move(zones))});
    }
    return reads;
  }
  template <class T, class Z>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Controls& op) const {
    const auto& l = *out.layout;
    if (axis == 0 || axis + 1 != l.rank || l.origin[axis] != 0 ||
        l.shape[axis] != dims[axis] || (dims[axis] != 3 && dims[axis] != 4) ||
        l.strides[axis] != sizeof(T) ||
        l.strides[axis - 1] != dims[axis] * sizeof(T)) {
      return false;
    }
    const unsigned run = axis - 1;
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < run; ++a)
      rows *= l.shape[a];
    // Validate every retained span before writing. Tile splitting of either
    // source coordinates or zones uses the general coordinate path.
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a)
        at[a] = l.origin[a];
      for (std::uint64_t row = 0; row < rows; ++row) {
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          const auto& x = reader.view(0, 0, at);
          auto q = at;
          q[axis] = 0;
          const ps_kernel_const_view_v8* z =
              op.copies ? nullptr : &reader.view(1, 0, q);
          auto n = std::min<std::uint64_t>(
              {1024, l.origin[run] + l.shape[run] - at[run],
               x.layout->origin[run] + x.layout->shape[run] - at[run]});
          if (z)
            n = std::min(
                n, z->layout->origin[run] + z->layout->shape[run] - at[run]);
          if (pass == 0) {
            if (x.layout->origin[axis] != 0 ||
                x.layout->shape[axis] != dims[axis] ||
                x.layout->strides[axis] != sizeof(T) ||
                x.layout->strides[run] != dims[axis] * sizeof(T) ||
                reinterpret_cast<std::uintptr_t>(address<T>(x, at)) %
                    alignof(T) ||
                reinterpret_cast<std::uintptr_t>(address<T>(out, at)) %
                    alignof(T)) {
              return false;
            }
            if (z &&
                (z->layout->origin[axis] != 0 || z->layout->shape[axis] != 3 ||
                 z->layout->strides[axis] != sizeof(Z) ||
                 z->layout->strides[run] != 3 * sizeof(Z) ||
                 reinterpret_cast<std::uintptr_t>(address<Z>(*z, q)) %
                     alignof(Z))) {
              return false;
            }
          } else {
            cancel(inv);
            span(address<T>(x, at), z ? address<Z>(*z, q) : nullptr,
                 address<T>(out, at), n, static_cast<unsigned>(dims[axis]), op);
          }
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
  template <class T, class Z>
  bool planar(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Controls& op) const {
    const auto& l = *out.layout;
    const unsigned run = l.rank - 1;
    if (axis == run || l.strides[run] != sizeof(T))
      return false;
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < run; ++a)
      rows *= l.shape[a];
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a)
        at[a] = l.origin[a];
      for (std::uint64_t row = 0; row < rows; ++row) {
        std::array<double, 3> o{};
        bool active = false;
        for (unsigned c = 0; c < components; ++c) {
          if (selected[c] == at[axis] && op.active[c]) {
            o = op.offsets[c];
            active = true;
          }
        }
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          const auto& x = reader.view(0, 0, at);
          const Z* zones[3]{};
          auto n = std::min<std::uint64_t>(
              {1024, l.origin[run] + l.shape[run] - at[run],
               x.layout->origin[run] + x.layout->shape[run] - at[run]});
          if (x.layout->strides[run] != sizeof(T) ||
              reinterpret_cast<std::uintptr_t>(address<T>(x, at)) %
                  alignof(T) ||
              reinterpret_cast<std::uintptr_t>(address<T>(out, at)) %
                  alignof(T))
            return false;
          if (active) {
            for (unsigned z = 0; z < 3; ++z) {
              auto q = at;
              q[axis] = z;
              const auto& v = reader.view(1, 0, q);
              zones[z] = address<Z>(v, q);
              if (v.layout->strides[run] != sizeof(Z) ||
                  reinterpret_cast<std::uintptr_t>(zones[z]) % alignof(Z))
                return false;
              n = std::min(
                  n, v.layout->origin[run] + v.layout->shape[run] - at[run]);
            }
          }
          if (pass) {
            cancel(inv);
            plane(address<T>(x, at), zones[0], zones[1], zones[2],
                  address<T>(out, at), n, o);
          }
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
  template <class T, class Z>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned output = 0; output < inv.output_count; ++output) {
      const auto& out = inv.outputs[output];
      const auto& l = *out.layout;
      Coordinate at{};
      std::uint64_t count = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        count *= l.shape[a];
        at[a] = l.origin[a];
      }
      if (!count)
        continue;
      const auto tuples = count / l.shape[axis];
      Controls op;
      op.count = components;
      std::copy(selected.begin(), selected.end(), op.selected.begin());
      unsigned requested = 0;
      for (unsigned c = 0; c < components; ++c) {
        requested += selected[c] >= l.origin[axis] &&
                     selected[c] - l.origin[axis] < l.shape[axis];
      }
      // Fixed numerical charge shared by v8 and v9; admit before any writes.
      work.poll(count + (requested ? tuples * (2 + 8 * requested) : 0) +
                3 * requested);
      for (unsigned c = 0; c < components; ++c) {
        if (selected[c] < l.origin[axis] ||
            selected[c] - l.origin[axis] >= l.shape[axis])
          continue;
        Coordinate q{};
        q[1] = c;
        for (unsigned z = 0; z < 3; ++z) {
          q[0] = z;
          op.offsets[c][z] = reader.value(2, 0, q);
        }
      }
      op.classify();
      if (packed<T, Z>(reader, inv, out, op) ||
          planar<T, Z>(reader, inv, out, op))
        continue;
      for (std::uint64_t i = 0; i < tuples; ++i) {
        if ((i & 63U) == 0)
          cancel(inv);
        std::array<double, 3> weights{};
        if (!op.copies) {
          auto q = at;
          for (unsigned z = 0; z < 3; ++z) {
            q[axis] = z;
            weights[z] = reader.value(1, 0, q);
          }
        }
        for (unsigned j = 0; j < l.shape[axis]; ++j) {
          at[axis] = l.origin[axis] + j;
          const auto raw = reader.raw(0, 0, at);
          auto result = raw;
          for (unsigned c = 0; c < components; ++c) {
            if (op.active[c] && selected[c] == at[axis]) {
              const auto d = op.delta(weights[0], weights[1], weights[2], c);
              if (d != 0) {
                T x;
                std::memcpy(&x, &raw, sizeof(T));
                result = convert(static_cast<double>(x) + d, sizeof(T) == 4);
              }
            }
          }
          store(out, at, result);
        }
        for (unsigned a = l.rank; a-- > 0;) {
          if (a == axis)
            continue;
          if (++at[a] < l.origin[a] + l.shape[a])
            break;
          at[a] = l.origin[a];
        }
      }
    }
    cancel(inv);
  }
  template <class T>
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& i,
                const Work& work) const {
    if (zone_type == ElementType::Float32)
      compute<T, float>(k, i, work);
    else
      compute<T, double>(k, i, work);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32)
      dispatch<float>(k, i, Work{i, work_limit});
    else
      dispatch<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    const Work work{i.views, work_limit, 0, i.work};
    if (type == ElementType::Float32)
      dispatch<float>(k, i.views, work);
    else
      dispatch<double>(k, i.views, work);
  }
};
OperationDefinition operation(bool opponent) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"axis", Type::Int64, false},
      {"components", Type::String, false},
      {"input_description", Type::String, false},
      {"group", Type::String, false}};
  if (opponent)
    parameters.push_back({"model", Type::String, false});
  auto definition = budgeted_operation(generation::operation(
      opponent ? "grade.color_balance_opponent" : "grade.color_balance_rgb", 3,
      std::move(parameters),
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [opponent](const auto& inputs, const auto& p) {
        return std::make_shared<Balance>(opponent, inputs, p);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade::balance
namespace ps::plugin_internal {
Status register_grade_color_balance(OperationRegistry* registry) {
  for (bool opponent : {false, true}) {
    auto status =
        registry->register_operation(grade::balance::operation(opponent));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
