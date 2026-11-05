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
#include "09-composite/nonseparable_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::nonseparable {
using namespace generation;  // NOLINT(build/namespaces)

inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return !d.extent; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
inline void advance(Coordinate* at, const ps_kernel_layout_v8& l, unsigned axis,
                    std::uint64_t n) {
  for (unsigned a = l.rank; a-- > 0 && n;) {
    if (a == axis)
      continue;
    n += (*at)[a] - l.origin[a];
    (*at)[a] = l.origin[a] + n % l.shape[a];
    n /= l.shape[a];
  }
}
/** Jointly contiguous packed pixels; an input crop never crosses a row gap. */
inline std::uint64_t dense(const ps_kernel_layout_v8& out,
                           const ps_kernel_layout_v8& in, const Coordinate& at,
                           unsigned width) {
  const unsigned axis = out.rank - 1;
  if (out.strides[axis] != width || in.strides[axis] != width ||
      in.origin[axis] != 0 || in.shape[axis] != 3)
    return 0;
  std::uint64_t n = 1, inner = 1;
  for (unsigned a = axis; a-- > 0;) {
    const auto step = static_cast<std::int64_t>(3 * width * inner);
    if ((out.shape[a] > 1 && out.strides[a] != step) ||
        (in.shape[a] > 1 && in.strides[a] != step))
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

template <Kind K>
struct Nonseparable final : Program {
  ElementType type;
  unsigned axis = 0;
  std::array<std::uint64_t, 3> channels{};
  bool packed = false;

  Nonseparable(bool lab, const std::vector<OperationMetadata>& in,
               const Parameters& p) {
    const auto& background = tensor(in.at(0), true);
    const auto& source = tensor(in.at(1), true);
    type = background.descriptor.element_type;
    require(floating(type) && source.descriptor.element_type == type,
            "NBL requires matching Float32/64");
    require(background.batch_axes == source.batch_axes &&
                background.descriptor.shape == source.descriptor.shape,
            "NBL requires matching grids and batch axes");
    if constexpr (radial(K)) {
      const auto& e = tensor(in.at(2));
      require(e.descriptor.element_type == type &&
                  e.descriptor.shape == std::vector<std::uint64_t>{1},
              "NBL neutral_threshold is a required T[1] input");
    }
    try {
      const auto roles = w3c(K)
                             ? std::vector<std::string>{"red", "green", "blue"}
                             : std::vector<std::string>{"l", "a", "b"};
      const auto b = color_point::select(in[0], p, roles);
      const auto s = color_point::select(in[1], p, roles);
      require(b.axis >= 0 && b.axis == s.axis && b.channels == s.channels &&
                  b.channels.size() == 3,
              "NBL requires matching explicit three-coordinate selectors");
      axis = static_cast<unsigned>(b.axis);
      const auto dims = background.sample_shape();
      require(dims[axis] == 3,
              "NBL color-only axis must contain exactly three coordinates");
      std::copy(b.channels.begin(), b.channels.end(), channels.begin());
      const auto& interpretation = b.interpretation;
      const char* model = w3c(K) ? "rgb" : lab ? "cielab" : "oklab";
      require(!b.semantic || interpretation.model == model,
              "NBL native color model mismatch");
      if (background.layout.spatial && source.layout.spatial)
        require(
            background.layout.height_axis == source.layout.height_axis &&
                background.layout.width_axis == source.layout.width_axis &&
                background.layout.channel_axis == source.layout.channel_axis,
            "NBL spatial axes disagree");
      if constexpr (w3c(K)) {
        const auto domain = generation::text(
            p, "working_domain",
            b.semantic
                ? (interpretation.transfer == "linear" ? "linear" : "encoded")
                : "");
        require(domain == "linear" || domain == "encoded",
                "NBL W3C requires explicit working_domain or description");
        require(!b.semantic || (!interpretation.transfer.empty() &&
                                (domain == "linear") ==
                                    (interpretation.transfer == "linear")),
                "NBL working_domain disagrees with description");
      } else if (lab) {
        require(p.count("source_white") || interpretation.white.has_value(),
                "NBL CIELAB requires explicit common source_white");
        if (p.count("source_white")) {
          const auto white =
              basis_ops::parse_white(generation::text(p, "source_white"));
          (void)basis_ops::white_xyz(white);
          require(!interpretation.white || *interpretation.white == white,
                  "NBL white disagrees with description");
        } else {
          (void)basis_ops::white_xyz(*interpretation.white);
        }
      }
      packed = axis + 1 == dims.size() &&
               channels == std::array<std::uint64_t, 3>{0, 1, 2};
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "NBL invalid white"};
    }
    auto output = *in[0].result_schema;
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
    std::vector<ReadRegion> reads;
    const auto included = [&](unsigned c) {
      const auto d = r.dimensions()[axis];
      return channels[c] >= d.offset && channels[c] - d.offset < d.extent;
    };
    const auto add = [&](unsigned port, unsigned c) {
      auto dims = r.dimensions();
      dims[axis] = {channels[c], 1};
      reads.push_back({port, 0, Region(std::move(dims))});
    };
    if constexpr (copy_only(K)) {
      for (unsigned c = 0; c < 3; ++c)
        if (included(c))
          add(K == Kind::Color ? (c == 0 ? 0 : 1) : (c == 0 ? 1 : 0), c);
    } else if constexpr (radial(K)) {
      if (included(0))
        add(0, 0);
      if (included(1) || included(2)) {
        for (unsigned port = 0; port < 2; ++port)
          for (unsigned c = 1; c < 3; ++c)
            add(port, c);
        reads.push_back({2, 0, Region({{0, 1}})});
      }
    } else {
      for (unsigned port = 0; port < 2; ++port)
        for (unsigned c = 0; c < 3; ++c)
          add(port, c);
    }
    return reads;
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned oi = 0; oi < inv.output_count; ++oi) {
      const auto& out = inv.outputs[oi];
      const auto& layout = *out.layout;
      Coordinate at{};
      std::uint64_t pixels = 1;
      for (unsigned a = 0; a < layout.rank; ++a) {
        at[a] = layout.origin[a];
        if (a != axis)
          pixels *= layout.shape[a];
      }
      if (!pixels || !layout.shape[axis])
        continue;
      constexpr unsigned kWork = copy_only(K) ? 1 : radial(K) ? 24 : 72;
      work.poll(pixels * layout.shape[axis] * kWork);
      const auto included = [&](unsigned c) {
        return channels[c] >= layout.origin[axis] &&
               channels[c] - layout.origin[axis] < layout.shape[axis];
      };
      const auto raw = [&](unsigned port, unsigned c) {
        auto position = at;
        position[axis] = channels[c];
        return reader.raw(port, 0, position);
      };
      const auto load = [&](unsigned port, unsigned c) {
        const auto bits = raw(port, c);
        T result;
        std::memcpy(&result, &bits, sizeof(T));
        return result;
      };
      T threshold = 0;
      if constexpr (radial(K)) {
        if (included(1) || included(2)) {
          const auto value = reader.raw(2, 0, Coordinate{});
          std::memcpy(&threshold, &value, sizeof(T));
        }
      }
      for (std::uint64_t done = 0; done < pixels;) {
        cancel(inv);
        std::uint64_t n = 0;
        if (packed && layout.origin[axis] == 0 && layout.shape[axis] == 3) {
          auto ba = at, sa = at;
          ba[axis] = K == Kind::Lightness ? 1 : 0;
          sa[axis] = K == Kind::Lightness || w3c(K) ? 0 : 1;
          const auto& bv = reader.view(0, 0, ba);
          const auto& sv = reader.view(1, 0, sa);
          n = std::min<std::uint64_t>(
              {copy_only(K) ? 524288U : 16384U, pixels - done,
               dense(layout, *bv.layout, at, sizeof(T)),
               dense(layout, *sv.layout, at, sizeof(T))});
          if (n) {
            auto position = at;
            position[axis] = 0;
            const auto* b = static_cast<const unsigned char*>(bv.allocation) +
                            offset(*bv.layout, bv.byte_offset, position);
            const auto* s = static_cast<const unsigned char*>(sv.allocation) +
                            offset(*sv.layout, sv.byte_offset, position);
            auto* target = static_cast<unsigned char*>(out.allocation) +
                           offset(layout, out.byte_offset, position);
            if (reinterpret_cast<std::uintptr_t>(b) % alignof(T) == 0 &&
                reinterpret_cast<std::uintptr_t>(s) % alignof(T) == 0 &&
                reinterpret_cast<std::uintptr_t>(target) % alignof(T) == 0) {
              span<K>(reinterpret_cast<const T*>(b),
                      reinterpret_cast<const T*>(s), threshold,
                      reinterpret_cast<T*>(target), n);
            } else {
              n = 0;
            }
          }
        }
        if (!n) {
          if constexpr (copy_only(K)) {
            for (unsigned c = 0; c < 3; ++c) {
              if (!included(c))
                continue;
              const unsigned port =
                  K == Kind::Color ? (c == 0 ? 0 : 1) : (c == 0 ? 1 : 0);
              auto position = at;
              position[axis] = channels[c];
              generation::store(out, position, raw(port, c));
            }
          } else {
            if constexpr (radial(K)) {
              if (included(0)) {
                auto position = at;
                position[axis] = channels[0];
                generation::store(out, position, raw(0, 0));
              }
            }
            if (w3c(K) || included(1) || included(2)) {
              Triple<T> b{}, s{};
              b.a = load(0, 1);
              b.b = load(0, 2);
              s.a = load(1, 1);
              s.b = load(1, 2);
              if constexpr (w3c(K)) {
                b.l = load(0, 0);
                s.l = load(1, 0);
              }
              const auto value = evaluate<K, Scalar<T>>(b, s, threshold);
              const std::array<T, 3> values{value.l, value.a, value.b};
              for (unsigned c = w3c(K) ? 0 : 1; c < 3; ++c) {
                if (!included(c))
                  continue;
                std::uint64_t bits = 0;
                std::memcpy(&bits, &values[c], sizeof(T));
                auto position = at;
                position[axis] = channels[c];
                generation::store(out, position, bits);
              }
            }
          }
          n = 1;
        }
        done += n;
        advance(&at, layout, axis, n);
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
OperationDefinition operation(bool lab, const char* key) {
  using P = OperationParameterType;
  std::vector<OperationParameterSpec> params{
      {"axis", P::Int64, false},
      {"components", P::String, false},
      {"input_description", P::String, false},
      {"group", P::String, false}};
  if (lab)
    params.push_back({"source_white", P::String, false});
  if constexpr (w3c(K))
    params.push_back({"working_domain", P::String, false});
  return budgeted_operation(generation::operation(
      key, radial(K) ? 3 : 2, std::move(params),
      {{"color", tensor_schema(ElementType::Float64, {1})}},
      [lab](const auto& inputs, const auto& parameters) {
        return std::make_shared<Nonseparable<K>>(lab, inputs, parameters);
      }));
}
}  // namespace ps::plugin_internal::nonseparable

namespace ps::plugin_internal {
Status register_nonseparable(OperationRegistry* registry) {
  using nonseparable::Kind;
  using nonseparable::operation;
  for (auto op : {operation<Kind::Hue>(true, "blend.hue_lab"),
                  operation<Kind::Hue>(false, "blend.hue_oklab"),
                  operation<Kind::HueW3c>(false, "blend.hue_w3c")}) {
    const auto status = registry->register_operation(std::move(op));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
