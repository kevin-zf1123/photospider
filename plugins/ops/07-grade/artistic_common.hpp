// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "07-grade/statistical_common.hpp"

namespace ps::plugin_internal::grade::artistic {
using namespace generation;  // NOLINT(build/namespaces)
using statistics::cancel;
using statistics::empty;
using statistics::output_like;
using statistics::selectors;
using statistics::span;
using statistics::Span;
using statistics::typed;
using statistics::whole;
using P = OperationParameterType;

// Fixed ABI parameters are scalars or strings. Arrays use comma-separated,
// row-major values; their lengths are structural and checked during prepare.
inline std::vector<std::string> tokens(const std::string& value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(',', start);
    auto part =
        value.substr(start, end == std::string::npos ? end : end - start);
    const auto first = part.find_first_not_of(" \t\r\n");
    const auto last = part.find_last_not_of(" \t\r\n");
    require(first != std::string::npos, "grading array contains an empty item");
    out.push_back(part.substr(first, last - first + 1));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return out;
}
inline std::vector<double> reals(const Parameters& p, const char* key,
                                 std::size_t size, const char* fallback = "") {
  const auto words = tokens(text(p, key, fallback));
  require(words.size() == size, "grading static array length");
  std::vector<double> out;
  out.reserve(size);
  for (const auto& word : words) {
    std::istringstream in(word);
    in.imbue(std::locale::classic());
    double v = 0;
    in >> v;
    require(!in.fail() && in.peek() == std::char_traits<char>::eof(),
            "grading static Float64 array encoding");
    out.push_back(v);
  }
  return out;
}
inline std::vector<std::int64_t> integers(const Parameters& p, const char* key,
                                          std::size_t size,
                                          const char* fallback = "") {
  if (!p.count(key) && !*fallback)
    return std::vector<std::int64_t>(size, 0);
  const auto words = tokens(text(p, key, fallback));
  require(words.size() == size, "grading static integer array length");
  std::vector<std::int64_t> out;
  out.reserve(size);
  for (const auto& word : words) {
    std::int64_t v = 0;
    const auto r = std::from_chars(word.data(), word.data() + word.size(), v);
    require(r.ec == std::errc{} && r.ptr == word.data() + word.size(),
            "grading static Int64 array encoding");
    out.push_back(v);
  }
  return out;
}
inline bool clamping(const Parameters& p, const char* fallback = "clamp") {
  const auto edge = text(p, "edge", fallback);
  require(edge == "clamp" || edge == "reject", "grading edge mode");
  return edge == "clamp";
}
inline void enum_parameter(const Parameters& p, const char* key, const char* a,
                           const char* b) {
  const auto v = text(p, key, a);
  require(v == a || v == b, "grading enum parameter");
}
inline std::uint64_t count(const ps_kernel_layout_v8& l) {
  std::uint64_t n = 1;
  for (unsigned a = 0; a < l.rank; ++a)
    n *= l.shape[a];
  return n;
}
inline Region region(const ps_kernel_layout_v8& l) {
  std::vector<RegionDimension> dims;
  for (unsigned a = 0; a < l.rank; ++a)
    dims.push_back({l.origin[a], l.shape[a]});
  return Region(std::move(dims));
}
template <class T, class View>
auto address(const View& v, const Coordinate& at) {
  using Byte = std::conditional_t<std::is_same_v<View, ps_kernel_const_view_v8>,
                                  const unsigned char, unsigned char>;
  using Pointer =
      std::conditional_t<std::is_same_v<View, ps_kernel_const_view_v8>,
                         const T*, T*>;
  return reinterpret_cast<Pointer>(static_cast<Byte*>(v.allocation) +
                                   offset(*v.layout, v.byte_offset, at));
}
template <class T>
inline bool aligned(const T* p) {
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
}
// Enumerate each logical tuple once, also for planar, reversed and component-
// sliced views. The component coordinate starts at the output slice origin.
template <class F>
void pixels(const ps_kernel_mutable_view_v8& out, unsigned axis, F f) {
  const auto& l = *out.layout;
  Coordinate at{};
  std::uint64_t n = 1;
  for (unsigned a = 0; a < l.rank; ++a) {
    at[a] = l.origin[a];
    if (a != axis)
      n *= l.shape[a];
  }
  for (std::uint64_t j = 0; j < n; ++j) {
    f(at, j);
    for (unsigned a = l.rank; a-- > 0;) {
      if (a == axis)
        continue;
      if (++at[a] < l.origin[a] + l.shape[a])
        break;
      at[a] = l.origin[a];
    }
  }
}
struct Image {
  ElementType type = ElementType::Float64;
  unsigned axis = 0, yaxis = 0, xaxis = 1;
  std::vector<std::uint64_t> dims, selected;
  color_point::Selection selection;
  Image() = default;
  Image(const OperationMetadata& input, const Parameters& p,
        const std::vector<std::string>& roles = {}, bool spatial = false,
        bool native = true) {
    const auto& source = tensor(input, !spatial);
    dims = source.sample_shape();
    type = source.descriptor.element_type;
    require(floating(type), "grading image requires Float32/64");
    try {
      if (roles.empty() && !p.count("input_description") && p.count("axis")) {
        require(!p.count("group"), "raw selection forbids group");
        const auto a = integer(p, "axis");
        require(a >= 0 && static_cast<std::uint64_t>(a) <
                              source.descriptor.shape.size(),
                "grading component axis");
        axis = a + source.batch_axes.size();
        selected = mask::channels(text(p, "components"), dims[axis]);
        selection.axis = axis;
        selection.channels = selected;
        if (source.layout.spatial)
          require(source.layout.channel_axis == std::optional<std::uint32_t>(a),
                  "grading spatial component axis");
      } else {
        selection = color_point::select(input, p, roles, native);
        require(selection.axis >= 0, "grading requires a component axis");
        axis = selection.axis;
        selected = selection.channels;
      }
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    }
    if (spatial) {
      require(dims.size() == 3, "grading spatial image requires rank three");
      unsigned k = 0;
      for (unsigned a = 0; a < 3; ++a)
        if (a != axis)
          (k++ == 0 ? yaxis : xaxis) = a;
      if (source.layout.spatial)
        require(source.layout.height_axis == yaxis &&
                    source.layout.width_axis == xaxis,
                "grading logical y,x axes");
    }
  }
  int component(std::uint64_t c) const {
    const auto i = std::find(selected.begin(), selected.end(), c);
    return i == selected.end() ? -1 : static_cast<int>(i - selected.begin());
  }
  bool active(const Region& r) const {
    if (empty(r))
      return false;
    const auto d = r.dimensions()[axis];
    for (auto c : selected)
      if (c >= d.offset && c - d.offset < d.extent)
        return true;
    return false;
  }
  std::vector<ReadRegion> support(const Region& r, bool coupled) const {
    if (empty(r))
      return {};
    std::vector<ReadRegion> out{{0, 0, r}};
    if (coupled && active(r)) {
      for (auto c : selected) {
        auto q = r.dimensions();
        q[axis] = {c, 1};
        out.push_back({0, 0, Region(std::move(q))});
      }
    }
    return out;
  }
  Coordinate spatial_coordinate(const Coordinate& at, unsigned c) const {
    return Coordinate{at[yaxis], at[xaxis], c};
  }
};
// Interleaved images use a direct span; each fragment is proved separately.
// A failed direct fragment falls back before that fragment writes any bytes.
template <class T, class Fast, class Slow>
void image_runs(const Image& image, const mask::Reader& r,
                const ps_kernel_invocation_v8& inv,
                const ps_kernel_mutable_view_v8& out, Fast fast, Slow slow,
                bool flatten = false) {
  const auto& l = *out.layout;
  const unsigned axis = image.axis;
  const auto c = image.dims[axis];
  if (axis == 0 || axis + 1 != l.rank || l.origin[axis] || l.shape[axis] != c ||
      l.strides[axis] != sizeof(T) || l.strides[axis - 1] != c * sizeof(T)) {
    pixels(out, axis, slow);
    return;
  }
  const unsigned run = axis - 1;
  if (flatten) {
    Coordinate first{};
    for (unsigned a = 0; a < l.rank; ++a)
      first[a] = l.origin[a];
    const auto& v = r.view(0, 0, first);
    std::uint64_t dense = sizeof(T);
    bool packed =
        aligned(address<T>(v, first)) && aligned(address<T>(out, first));
    for (unsigned a = l.rank; a-- > 0;) {
      packed &=
          v.layout->origin[a] <= l.origin[a] &&
          l.origin[a] + l.shape[a] <= v.layout->origin[a] + v.layout->shape[a];
      if (l.shape[a] > 1)
        packed &= l.strides[a] == static_cast<std::int64_t>(dense) &&
                  v.layout->strides[a] == static_cast<std::int64_t>(dense);
      dense *= l.shape[a];
    }
    if (packed) {
      const auto n = dense / (sizeof(T) * c);
      const auto* src = address<T>(v, first);
      auto* dst = address<T>(out, first);
      bool done = true;
      for (std::uint64_t j = 0; j < n; j += 1024) {
        cancel(inv);
        auto at = first;
        auto remaining = j;
        for (unsigned a = axis; a-- > 0;) {
          at[a] += remaining % l.shape[a];
          remaining /= l.shape[a];
        }
        if (!fast(src + j * c, dst + j * c,
                  std::min<std::uint64_t>(1024, n - j), at)) {
          done = false;
          break;
        }
      }
      if (done)
        return;
    }
  }
  Coordinate at{};
  std::uint64_t rows = 1;
  for (unsigned a = 0; a < l.rank; ++a) {
    at[a] = l.origin[a];
    if (a < run)
      rows *= l.shape[a];
  }
  for (std::uint64_t row = 0; row < rows; ++row) {
    for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
      cancel(inv);
      const auto& v = r.view(0, 0, at);
      const auto n = std::min<std::uint64_t>(
          {1024, l.origin[run] + l.shape[run] - at[run],
           v.layout->origin[run] + v.layout->shape[run] - at[run]});
      const bool packed =
          !v.layout->origin[axis] && v.layout->shape[axis] == c &&
          v.layout->strides[axis] == sizeof(T) &&
          v.layout->strides[run] == c * sizeof(T) &&
          aligned(address<T>(v, at)) && aligned(address<T>(out, at));
      if (!packed || !fast(address<T>(v, at), address<T>(out, at), n, at)) {
        auto q = at;
        for (std::uint64_t j = 0; j < n; ++j) {
          q[run] = at[run] + j;
          slow(q, j);
        }
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
}  // namespace ps::plugin_internal::grade::artistic
