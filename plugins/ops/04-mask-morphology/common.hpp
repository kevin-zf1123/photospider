#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/runtime.hpp"
#include "03-generation/scalar_math.hpp"

namespace ps::plugin_internal::mask {
using generation::add_output;
using generation::bits;
using generation::check;
using generation::convert;
using generation::Coordinate;
using generation::floating;
using generation::integer;
using generation::Kernel;
using generation::member;
using generation::number;
using generation::offset;
using generation::Parameters;
using generation::Program;
using generation::ReadRegion;
using generation::real;
using generation::require;
using generation::samples;
using generation::shape;
using generation::size_product;
using generation::store;
using generation::take;
using generation::tensor;
using generation::tensor_schema;
using generation::text;
using generation::Work;
using Parameter = OperationParameterSpec;
using Type = OperationParameterType;
using Exact = model_ops::RationalMathStorage<384>;
using Rational = Exact::Rational;

inline void parameter(bool valid, const char* message) {
  if (!valid)
    throw Status{ErrorCode::InvalidArgument, message,
                 FailureReason::InvalidDomain};
}
inline std::uint64_t bytes(unsigned __int128 n) {
  parameter(n <= static_cast<unsigned __int128>(PTRDIFF_MAX),
            "mask scratch capacity overflow");
  return static_cast<std::uint64_t>(n);
}
inline bool boolean(const Parameters& p, const char* key) {
  return std::get<bool>(p.at(key));
}
inline ElementType output_dtype(const Parameters& p) {
  const auto name = text(p, "output_dtype");
  parameter(name == "float32" || name == "float64", "mask output_dtype");
  return name == "float32" ? ElementType::Float32 : ElementType::Float64;
}
inline void spatial_axes(const ResultTensorSpec& s, unsigned rank) {
  if (s.layout.spatial)
    require(s.layout.height_axis == rank - 2 &&
                s.layout.width_axis == rank - 1 &&
                s.layout.channel_axis == (rank == 3
                                              ? std::optional<std::uint32_t>{0}
                                              : std::optional<std::uint32_t>{}),
            "mask requires planar spatial axes");
}
inline const ResultTensorSpec& image(const OperationMetadata& input,
                                     unsigned rank, bool raw_byte = false) {
  const auto& s = tensor(input);
  require(s.descriptor.shape.size() == rank &&
              (raw_byte ? s.descriptor.element_type == ElementType::UInt8
                        : floating(s.descriptor.element_type)),
          "mask input rank/dtype");
  spatial_axes(s, rank);
  return s;
}
inline void same(const OperationMetadata& a, const OperationMetadata& b) {
  const auto& x = tensor(a);
  const auto& y = image(b, static_cast<unsigned>(x.descriptor.shape.size()));
  require(x.sample_shape() == y.sample_shape() &&
              x.descriptor.element_type == y.descriptor.element_type,
          "mask input shape/dtype mismatch");
}
inline SchemaTemplate spatial_schema(ElementType type,
                                     const std::vector<std::uint64_t>& dims) {
  auto s = tensor_schema(type, dims, true);
  if (dims.size() == 3) {
    s.tensors[0].layout.channel_axis = 0;
    s.tensors[0].layout.height_axis = 1;
    s.tensors[0].layout.width_axis = 2;
  }
  check(s.validate());
  return s;
}
inline std::vector<std::uint64_t> channels(const std::string& value,
                                           std::uint64_t count) {
  std::vector<std::uint64_t> result;
  std::size_t pos = 0;
  parameter(!value.empty(), "empty mask channel selector");
  while (pos < value.size()) {
    const auto end = value.find(',', pos);
    const auto size = (end == std::string::npos ? value.size() : end) - pos;
    std::uint64_t channel = 0;
    const auto parsed =
        std::from_chars(value.data() + pos, value.data() + pos + size, channel);
    parameter(size && (size == 1 || value[pos] != '0') &&
                  parsed.ec == std::errc{} &&
                  parsed.ptr == value.data() + pos + size &&
                  value[pos] >= '0' && value[pos] <= '9',
              "noncanonical mask channel selector");
    require(channel < count, "mask channel outside axis");
    parameter(std::find(result.begin(), result.end(), channel) == result.end(),
              "duplicate mask channel");
    result.push_back(channel);
    if (end == std::string::npos)
      break;
    pos = end + 1;
    parameter(pos < value.size(), "trailing mask channel separator");
  }
  return result;
}
inline Region spatial_region(const Region& q) {
  const auto& d = q.dimensions();
  return Region({d[d.size() - 2], d.back()});
}
inline Region channel_region(std::uint64_t c, const Region& q) {
  const auto& d = q.dimensions();
  return Region({{c, 1}, d[d.size() - 2], d.back()});
}

// The cache accelerates fixed-view addressing only. Preparation has proved
// every requested coordinate's membership; no payload condition is checked
// here.
struct Reader final {
  const Kernel& kernel;
  const ps_kernel_invocation_v8& invocation;
  mutable std::array<const ps_kernel_const_view_v8*, 16> cached{};
  const ps_kernel_const_view_v8& view(unsigned port, unsigned field,
                                      const Coordinate& at) const {
    const auto index = kernel.input_base[port] + field;
    const auto& group = kernel.groups[index];
    auto* v = cached[index];
    bool inside = v != nullptr;
    for (unsigned a = 0; inside && a < group.grid.rank(); ++a)
      inside = at[a] >= v->layout->origin[a] &&
               at[a] - v->layout->origin[a] < v->layout->shape[a];
    if (!inside) {
      const auto tile = fixed_tensor::input_tile(group.grid, at);
      const auto slot =
          group.slots.size() == 1
              ? group.slots[0].second
              : std::lower_bound(
                    group.slots.begin(), group.slots.end(), tile,
                    [](const auto& p, TileIndex t) { return p.first < t; })
                    ->second;
      cached[index] = v = &invocation.inputs[slot];
    }
    return *v;
  }
  std::uint64_t raw(unsigned port, unsigned field, const Coordinate& at) const {
    const auto& v = view(port, field, at);
    std::uint64_t result = 0;
    std::memcpy(
        &result,
        static_cast<const std::uint8_t*>(v.allocation) +
            offset(*v.layout, v.byte_offset, at),
        generation::width(static_cast<ElementType>(v.layout->element_type)));
    return result;
  }
  double value(unsigned port, unsigned field, const Coordinate& at) const {
    const auto data = raw(port, field, at);
    if (kernel.groups[kernel.input_base[port] + field].type ==
        ElementType::Float32) {
      const auto raw32 = static_cast<std::uint32_t>(data);
      float v;
      std::memcpy(&v, &raw32, 4);
      return v;
    }
    return number(data);
  }
  Rational exact(const Exact& e, unsigned port, unsigned field,
                 const Coordinate& at) const {
    return e.binary(raw(port, field, at),
                    kernel.groups[kernel.input_base[port] + field].type ==
                        ElementType::Float32);
  }
};
template <class T>
T load(const void* base, std::ptrdiff_t stride, std::uint64_t index) {
  T value;
  std::memcpy(&value,
              static_cast<const std::uint8_t*>(base) +
                  stride * static_cast<std::ptrdiff_t>(index),
              sizeof(T));
  return value;
}
template <class T>
void save(void* base, std::ptrdiff_t stride, std::uint64_t index, T value) {
  std::memcpy(static_cast<std::uint8_t*>(base) +
                  stride * static_cast<std::ptrdiff_t>(index),
              &value, sizeof(T));
}
struct ExactArena final {
  Exact* p;
  ExactArena(void* data, const Work& work)
      : p(new (data) Exact(numeric_ops::SequenceProfile::Strict)) {
    p->bind(work);
  }
  ~ExactArena() { p->~Exact(); }
};
template <class Arithmetic>
inline std::uint64_t rounded(Arithmetic& e, typename Arithmetic::Rational value,
                             bool narrow) {
  if (Arithmetic::zero(value))
    value.negative = false;
  const auto result = e.rounded(value, narrow);
  check(e.status());
  return result;
}
template <class Arithmetic>
inline typename Arithmetic::Rational curve01(Arithmetic& e,
                                             typename Arithmetic::Rational t,
                                             bool smooth) {
  const auto zero = e.integer(0), one = e.integer(1);
  if (e.order(t, zero) <= 0)
    return zero;
  if (e.order(t, one) >= 0)
    return one;
  return smooth
             ? e.multiply(e.multiply(t, t),
                          e.subtract(e.integer(3), e.multiply(e.integer(2), t)))
             : t;
}
template <class Arithmetic>
inline typename Arithmetic::Rational soft_edge(
    Arithmetic& e, const typename Arithmetic::Rational& x, double threshold,
    double width, bool smooth, bool upper = false) {
  const auto t = e.binary(bits(threshold));
  if (width == 0)
    return e.integer(upper ? e.order(x, t) <= 0 : e.order(x, t) >= 0);
  const auto w = e.binary(bits(width));
  const auto q =
      e.divide(e.add(e.subtract(x, t), e.divide(w, e.integer(2))), w);
  const auto f = curve01(e, q, smooth);
  return upper ? e.subtract(e.integer(1), f) : f;
}
inline std::vector<Parameter> footprint_parameters() {
  return {{"footprint", Type::String},
          {"radius", Type::Int64, false},
          {"offsets", Type::String, false}};
}
struct Footprint final {
  std::string kind;
  std::int64_t ry = 0, rx = 0;
  std::vector<std::pair<std::int64_t, std::int64_t>> custom;
  explicit Footprint(const Parameters& p);
  template <class F>
  void each(F f) const {
    if (kind == "custom") {
      for (auto b : custom)
        f(b.first, b.second);
      return;
    }
    for (std::int64_t y = -ry; y <= ry; ++y) {
      const auto extent = row_radius(y);
      for (std::int64_t x = -extent; x <= extent; ++x)
        f(y, x);
    }
  }
  std::int64_t row_radius(std::int64_t y) const;
  Region support(const Region& q, std::uint64_t h, std::uint64_t w,
                 unsigned stages = 1) const;
};
OperationDefinition definition(const char* name, unsigned ports,
                               std::vector<Parameter> parameters,
                               generation::Prepare prepare,
                               bool valid_output = false);
void validate_description(const OperationMetadata& input, const Parameters& p,
                          const std::vector<std::uint64_t>& selected,
                          bool required, bool lab = false, bool hue = false);
}  // namespace ps::plugin_internal::mask
