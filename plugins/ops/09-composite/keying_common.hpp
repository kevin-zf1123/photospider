// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "09-composite/keying_math.hpp"

namespace ps::plugin_internal::keying {
using namespace generation;  // NOLINT(build/namespaces)

inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context)) {
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
}
inline void fail(const char* message) {
  throw Status{ErrorCode::OperationFailed, message};
}
inline SchemaTemplate output_schema(const OperationMetadata& m) {
  auto s = *m.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  s.tensors[0].key = "samples";
  s.tensors[0].atomic_trailing_axes = 0;
  check(s.validate());
  return s;
}
inline void check_shape(const OperationMetadata& m,
                        const std::vector<std::uint64_t>& s, ElementType t) {
  const auto& a = tensor(m);
  require(a.sample_shape() == s && a.descriptor.element_type == t,
          "KEY input shape/dtype mismatch");
}
inline SchemaTemplate plane_schema(ElementType type, std::uint64_t h,
                                   std::uint64_t w) {
  return tensor_schema(type, {h, w}, true);
}
inline std::vector<OperationParameterSpec> color_parameters(bool lab = false) {
  using P = OperationParameterType;
  std::vector<OperationParameterSpec> p{{"axis", P::Int64, false},
                                        {"components", P::String, false},
                                        {"input_description", P::String, false},
                                        {"group", P::String, false}};
  p.push_back({lab ? "source_white" : "source_basis", P::String, false});
  return p;
}

struct Color final {
  unsigned axis = 0;
  std::array<std::uint64_t, 3> channels{};
  std::uint64_t count = 3, alpha = UINT64_MAX;
  std::array<double, 3> weights{};
  bool packed = false;
  Color() = default;
  Color(const OperationMetadata& m, const Parameters& p, bool lab = false,
        bool alpha_required = false, bool basis_required = false) {
    try {
      const auto s = color_point::select(
          m, p,
          lab ? std::vector<std::string>{"l", "a", "b"}
              : std::vector<std::string>{"red", "green", "blue"});
      require(s.axis >= 0 && s.channels.size() == 3,
              "KEY requires explicit three-component selection");
      axis = static_cast<unsigned>(s.axis);
      const auto dims = tensor(m).sample_shape();
      count = dims[axis];
      std::copy(s.channels.begin(), s.channels.end(), channels.begin());
      const auto& i = s.interpretation;
      require(!s.semantic || i.model == (lab ? "cielab" : "rgb"),
              "KEY color model mismatch");
      if (!lab) {
        require(!s.semantic ||
                    (i.transfer == "linear" &&
                     (i.association.empty() || i.association == "straight")),
                "KEY requires straight relative linear RGB");
      }
      if (lab) {
        require(p.count("source_white") || i.white,
                "KEY CIELAB requires an explicit common white");
        const auto white =
            p.count("source_white")
                ? basis_ops::parse_white(generation::text(p, "source_white"))
                : *i.white;
        (void)basis_ops::white_xyz(white);
        require(!i.white || *i.white == white, "KEY white disagreement");
      } else if (basis_required || p.count("source_basis") ||
                 !i.primaries.empty() || i.primaries_xy) {
        format::RgbBasis assertion;
        assertion.preset = i.primaries;
        assertion.primaries_xy = i.primaries_xy;
        assertion.white = i.white;
        const auto basis =
            p.count("source_basis")
                ? basis_ops::parse_basis(generation::text(p, "source_basis"))
                : basis_ops::resolve(assertion);
        require(i.primaries.empty() ||
                    basis_ops::preset(i.primaries).xy == basis.xy,
                "KEY basis disagreement");
        require(!i.primaries_xy || *i.primaries_xy == basis.xy,
                "KEY primaries disagreement");
        require(!i.white || *i.white == basis.white, "KEY white disagreement");
        weights =
            basis_ops::ExactRow(basis_ops::rgb_matrix(basis.xy, basis.white), 1)
                .approximate;
        const auto sum = (weights[0] + weights[1]) + weights[2];
        for (auto& weight : weights) {
          weight /= sum;
        }
      }
      if (alpha_required) {
        if (s.semantic) {
          require(!p.count("alpha_index"),
                  "KEY description forbids alpha_index");
          for (const auto& g : s.description->groups) {
            if (g.name == generation::text(p, "group") && g.alpha) {
              alpha = *g.alpha;
              if (alpha < s.description->channels.size()) {
                color_point::native_encoding(
                    s.description->channels[alpha].encoding);
              }
            }
          }
        } else {
          require(p.count("alpha_index") && integer(p, "alpha_index", -1) >= 0,
                  "KEY requires explicit alpha_index");
          alpha = static_cast<std::uint64_t>(integer(p, "alpha_index"));
        }
        require(alpha < count && std::find(channels.begin(), channels.end(),
                                           alpha) == channels.end(),
                "KEY invalid alpha selection");
      }
      packed = axis + 1 == dims.size() &&
               channels == std::array<std::uint64_t, 3>{0, 1, 2};
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "KEY invalid color basis"};
    }
  }
};

template <class T>
inline T load(const mask::Reader& reader, unsigned port, Coordinate at) {
  const auto raw = reader.raw(port, 0, at);
  T value;
  std::memcpy(&value, &raw, sizeof(T));
  return value;
}
template <class T>
inline void save(const ps_kernel_mutable_view_v8& out, Coordinate at, T value) {
  std::uint64_t raw = 0;
  std::memcpy(&raw, &value, sizeof(T));
  store(out, at, raw);
}
template <class T>
inline const T* pointer(const mask::Reader& reader, unsigned port,
                        Coordinate at, unsigned xaxis, std::uint64_t pixel,
                        std::uint64_t* n) {
  const auto& v = reader.view(port, 0, at);
  if (v.layout->strides[xaxis] !=
      static_cast<std::int64_t>(pixel * sizeof(T))) {
    return nullptr;
  }
  const auto* p = static_cast<const unsigned char*>(v.allocation) +
                  offset(*v.layout, v.byte_offset, at);
  if (reinterpret_cast<std::uintptr_t>(p) % alignof(T)) {
    return nullptr;
  }
  *n = std::min(*n,
                v.layout->origin[xaxis] + v.layout->shape[xaxis] - at[xaxis]);
  return reinterpret_cast<const T*>(p);
}
inline Coordinate insert_axis(const Coordinate& in, unsigned axis,
                              std::uint64_t c = 0) {
  Coordinate result{};
  for (unsigned j = 0; j < 8; ++j) {
    result[j] = j == axis ? c : in[j < axis ? j : j - 1];
  }
  return result;
}
inline Coordinate remove_axis(const Coordinate& in, unsigned axis) {
  Coordinate result{};
  for (unsigned j = 0; j + 1 < 8; ++j) {
    result[j] = in[j < axis ? j : j + 1];
  }
  return result;
}
inline void advance(Coordinate* at, const ps_kernel_layout_v8& l, unsigned skip,
                    std::uint64_t n) {
  for (unsigned a = l.rank; a-- > 0 && n;) {
    if (a == skip) {
      continue;
    }
    n += (*at)[a] - l.origin[a];
    (*at)[a] = l.origin[a] + n % l.shape[a];
    n /= l.shape[a];
  }
}

struct Arena final {
  unsigned char* p;
  explicit Arena(const ps_kernel_invocation_v8& inv)
      : p(static_cast<unsigned char*>(inv.scratch[0].allocation)) {}
  template <class T>
  T* take(std::uint64_t n) {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    p += (alignof(T) - a % alignof(T)) % alignof(T);
    auto* answer = reinterpret_cast<T*>(p);
    p += n * sizeof(T);
    return answer;
  }
};

template <class T>
const T* direct_input(const mask::Reader& reader, unsigned port,
                      const std::vector<std::uint64_t>& dims) {
  const auto& group = reader.kernel.groups[reader.kernel.input_base[port]];
  if (group.slots.size() != 1) {
    return nullptr;
  }
  const auto& v = reader.invocation.inputs[group.slots[0].second];
  std::uint64_t stride = sizeof(T);
  for (unsigned a = dims.size(); a-- > 0;) {
    if (v.layout->origin[a] || v.layout->shape[a] != dims[a] ||
        (dims[a] > 1 &&
         v.layout->strides[a] != static_cast<std::int64_t>(stride))) {
      return nullptr;
    }
    stride *= dims[a];
  }
  const auto* p =
      static_cast<const unsigned char*>(v.allocation) + v.byte_offset;
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0
             ? reinterpret_cast<const T*>(p)
             : nullptr;
}
template <class T>
T* direct_output(const ps_kernel_mutable_view_v8& v,
                 const std::vector<std::uint64_t>& dims) {
  if (v.layout->rank != dims.size()) {
    return nullptr;
  }
  std::uint64_t stride = sizeof(T);
  for (unsigned a = dims.size(); a-- > 0;) {
    if (v.layout->origin[a] || v.layout->shape[a] != dims[a] ||
        (dims[a] > 1 &&
         v.layout->strides[a] != static_cast<std::int64_t>(stride))) {
      return nullptr;
    }
    stride *= dims[a];
  }
  auto* p = static_cast<unsigned char*>(v.allocation) + v.byte_offset;
  return reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0
             ? reinterpret_cast<T*>(p)
             : nullptr;
}

inline std::uint64_t dense_window(const ps_kernel_layout_v8& l,
                                  const std::vector<std::uint64_t>& dims,
                                  const Coordinate& at, unsigned bytes) {
  std::uint64_t length = 1, inner = 1;
  for (unsigned a = l.rank; a-- > 0;) {
    if (l.shape[a] > 1 &&
        l.strides[a] != static_cast<std::int64_t>(bytes * inner)) {
      break;
    }
    length += (l.origin[a] + l.shape[a] - at[a] - 1) * inner;
    if (l.origin[a] || l.shape[a] != dims[a]) {
      break;
    }
    inner *= dims[a];
  }
  return length;
}

/** Dense direct input or a fixed-view gather, with declared invocation scratch.
 */
template <class T>
const T* gather(const mask::Reader& reader, unsigned port,
                const std::vector<std::uint64_t>& dims, T* buffer,
                const Work& work) {
  const auto& group = reader.kernel.groups[reader.kernel.input_base[port]];
  if (group.slots.size() == 1) {
    const auto& v = reader.invocation.inputs[group.slots[0].second];
    std::uint64_t stride = sizeof(T);
    bool dense = true;
    for (unsigned a = dims.size(); a-- > 0;) {
      dense &= v.layout->origin[a] == 0 && v.layout->shape[a] == dims[a] &&
               (dims[a] == 1 ||
                v.layout->strides[a] == static_cast<std::int64_t>(stride));
      stride *= dims[a];
    }
    const auto* address =
        static_cast<const unsigned char*>(v.allocation) + v.byte_offset;
    if (dense && reinterpret_cast<std::uintptr_t>(address) % alignof(T) == 0) {
      return reinterpret_cast<const T*>(address);
    }
  }
  Coordinate at{};
  const auto rank = static_cast<unsigned>(dims.size());
  const auto total = size_product(dims);
  for (std::uint64_t i = 0; i < total;) {
    const auto& view = reader.view(port, 0, at);
    const auto n = std::min<std::uint64_t>(
        {4096, total - i, dense_window(*view.layout, dims, at, sizeof(T))});
    work.poll(n);
    const auto* source = static_cast<const unsigned char*>(view.allocation) +
                         offset(*view.layout, view.byte_offset, at);
    std::memcpy(buffer + i, source, n * sizeof(T));
    i += n;
    auto remaining = n;
    for (unsigned a = rank; a-- > 0 && remaining;) {
      remaining += at[a];
      at[a] = remaining % dims[a];
      remaining /= dims[a];
    }
  }
  return buffer;
}
template <class T>
void scatter(const T* values, const std::vector<std::uint64_t>& dims,
             const ps_kernel_mutable_view_v8& out, const Work& work) {
  const auto& l = *out.layout;
  Coordinate at{};
  std::uint64_t count = 1;
  for (unsigned a = 0; a < l.rank; ++a) {
    at[a] = l.origin[a];
    count *= l.shape[a];
  }
  for (std::uint64_t done = 0; done < count;) {
    std::uint64_t index = 0;
    for (unsigned a = 0; a < l.rank; ++a) {
      index = index * dims[a] + at[a];
    }
    const auto n = std::min<std::uint64_t>(
        {4096, count - done, dense_window(l, dims, at, sizeof(T))});
    work.poll(n);
    auto* p = static_cast<unsigned char*>(out.allocation) +
              offset(l, out.byte_offset, at);
    std::memcpy(p, values + index, n * sizeof(T));
    done += n;
    advance(&at, l, l.rank, n);
  }
}
}  // namespace ps::plugin_internal::keying
