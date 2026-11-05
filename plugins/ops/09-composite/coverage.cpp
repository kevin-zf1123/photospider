// Copyright 2026 Photospider Authors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "09-composite/coverage_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::coverage {
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

// Maximal common dense run, stopping before a cropped input row gap.
inline std::uint64_t dense(const ps_kernel_layout_v8& out,
                           const ps_kernel_layout_v8* in, const Coordinate& at,
                           std::uint64_t width) {
  std::uint64_t n = 1, inner = 1;
  for (unsigned a = out.rank; a-- > 0;) {
    if ((out.shape[a] > 1 &&
         out.strides[a] != static_cast<std::int64_t>(width * inner)) ||
        (in && in->shape[a] > 1 &&
         in->strides[a] != static_cast<std::int64_t>(width * inner)))
      break;
    const auto end = in ? std::min(out.origin[a] + out.shape[a],
                                   in->origin[a] + in->shape[a])
                        : out.origin[a] + out.shape[a];
    n += (end - at[a] - 1) * inner;
    if (in && (in->origin[a] > out.origin[a] ||
               in->origin[a] + in->shape[a] < out.origin[a] + out.shape[a]))
      break;
    inner *= out.shape[a];
  }
  return n;
}

template <Kind K>
struct Coverage final : Program {
  ElementType type;
  unsigned axis = 0;
  std::uint64_t alpha = 0, channels = 0;
  std::vector<std::uint64_t> colors;
  bool packed = false;
  static constexpr bool kCopy = K == Kind::Source || K == Kind::Destination;
  static constexpr bool kPrimitive = kCopy || K == Kind::Clear;

  Coverage(const std::vector<OperationMetadata>& in, const Parameters& p,
           bool encoded) {
    const auto& main = tensor(in.at(0), true);
    type = main.descriptor.element_type;
    require(floating(type), "CMP requires Float32/64");
    const auto dims = main.sample_shape();
    if constexpr (!kPrimitive) {
      const auto& back = tensor(in.at(1), true);
      require(main.descriptor.shape == back.descriptor.shape &&
                  main.batch_axes == back.batch_axes &&
                  type == back.descriptor.element_type,
              "CMP image shape/batch/dtype mismatch");
      try {
        auto select = color_point::select(in[0], p);
        require(select.axis >= 0 && (select.channels.size() == 1 ||
                                     select.channels.size() == 3),
                "CMP requires an explicit Gray or RGB component axis");
        if (select.channels.size() == 3)
          select = color_point::select(in[0], p, {"red", "green", "blue"});
        const auto other = color_point::select(
            in[1], p,
            select.channels.size() == 3
                ? std::vector<std::string>{"red", "green", "blue"}
                : std::vector<std::string>{});
        require(select.axis == other.axis && select.channels == other.channels,
                "CMP selectors disagree");
        axis = static_cast<unsigned>(select.axis);
        colors = select.channels;
        channels = dims[axis];
        if (select.semantic) {
          require(!p.count("alpha_index"),
                  "CMP description forbids raw alpha_index");
          bool found = false;
          for (const auto& group : select.description->groups)
            if (group.name == generation::text(p, "group") && group.alpha) {
              alpha = *group.alpha;
              if (alpha < select.description->channels.size())
                color_point::native_encoding(
                    select.description->channels[alpha].encoding);
              found = true;
            }
          require(found, "CMP color group requires alpha");
          const auto& i = select.interpretation;
          require(i.model == (colors.size() == 1 ? "gray" : "rgb"),
                  "CMP requires native Gray/RGB");
          require(!i.transfer.empty() && (i.transfer == "linear") != encoded,
                  "CMP working domain disagrees with description");
          require(i.association.empty() || i.association == "straight",
                  "CMP requires straight colors");
        } else {
          require(p.count("alpha_index") && integer(p, "alpha_index", -1) >= 0,
                  "CMP requires explicit alpha_index");
          alpha = static_cast<std::uint64_t>(integer(p, "alpha_index"));
        }
        require(alpha < channels && std::find(colors.begin(), colors.end(),
                                              alpha) == colors.end(),
                "CMP alpha must be separate from colors");
      } catch (const basis_ops::CheckedFailure& e) {
        throw e.status;
      }
      require(!main.layout.spatial || !back.layout.spatial ||
                  (main.layout.height_axis == back.layout.height_axis &&
                   main.layout.width_axis == back.layout.width_axis),
              "CMP spatial axes disagree");
      if constexpr (K == Kind::BlendedOver) {
        const auto& f = tensor(in.at(2), true);
        auto expected = main.descriptor.shape;
        expected[axis - main.batch_axes.size()] = colors.size();
        require(f.descriptor.element_type == type &&
                    f.batch_axes == main.batch_axes &&
                    f.descriptor.shape == expected,
                "CMP blended shape/dtype mismatch");
        require(!f.layout.spatial ||
                    (f.layout.channel_axis &&
                     *f.layout.channel_axis + f.batch_axes.size() == axis &&
                     (!main.layout.spatial ||
                      (f.layout.height_axis == main.layout.height_axis &&
                       f.layout.width_axis == main.layout.width_axis))),
                "CMP blended spatial axes disagree");
      }
      packed = axis + 1 == dims.size() && alpha == colors.size() &&
               channels == colors.size() + 1;
      for (unsigned c = 0; c < colors.size(); ++c)
        packed &= colors[c] == c;
    }
    auto output = *in[kPrimitive ? 0 : 1].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    if constexpr (K == Kind::PlusRaw) {
      auto& t = output.tensors[0];
      t.descriptor.shape[axis - t.batch_axes.size()] = colors.size();
      check(output.validate());
      add_output(this, output);
      t.descriptor.shape[axis - t.batch_axes.size()] = 1;
    }
    check(output.validate());
    add_output(this, std::move(output));
  }

  std::vector<ReadRegion> support(unsigned output,
                                  const Region& r) const override {
    if (empty(r) || K == Kind::Clear)
      return {};
    if constexpr (kCopy)
      return {{0, 0, r}};
    std::vector<ReadRegion> reads;
    const auto add = [&](unsigned port, std::uint64_t c, std::uint64_t n = 1) {
      auto d = r.dimensions();
      d[axis] = {c, n};
      reads.push_back({port, 0, Region(std::move(d))});
    };
    const auto d = r.dimensions()[axis];
    const auto includes = [&](std::uint64_t c) {
      return c >= d.offset && c - d.offset < d.extent;
    };
    bool color = false, alpha_needed = false;
    if constexpr (K == Kind::PlusRaw) {
      alpha_needed = true;
      if (output == 0) {
        for (auto c = d.offset; c < d.offset + d.extent; ++c) {
          add(0, colors[c]);
          add(1, colors[c]);
        }
      }
    } else {
      for (unsigned c = 0; c < colors.size(); ++c)
        if (includes(colors[c])) {
          color = true;
          if constexpr (source_color(K))
            add(0, colors[c]);
          if constexpr (background_color(K))
            add(1, colors[c]);
          if constexpr (K == Kind::BlendedOver)
            add(2, c);
        }
      alpha_needed = color || includes(alpha);
      // Copy AOV intervals independently, without adding other color planes.
      auto selected = colors;
      selected.push_back(alpha);
      std::sort(selected.begin(), selected.end());
      auto start = d.offset;
      for (auto c : selected) {
        if (!includes(c))
          continue;
        if (c > start)
          add(1, start, c - start);
        start = c + 1;
      }
      if (start < d.offset + d.extent)
        add(1, start, d.offset + d.extent - start);
    }
    if (alpha_needed) {
      if (color || K != Kind::SourceAtop)
        add(0, alpha);
      if (color || K != Kind::DestinationAtop)
        add(1, alpha);
    }
    return reads;
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      Coordinate at{};
      std::uint64_t count = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        at[a] = l.origin[a];
        count *= l.shape[a];
      }
      if (!count)
        continue;
      work.poll(count * (kPrimitive ? 1 : 32));
      if constexpr (kPrimitive) {
        for (std::uint64_t done = 0; done < count;) {
          cancel(inv);
          const ps_kernel_const_view_v8* v = nullptr;
          if constexpr (kCopy)
            v = &reader.view(0, 0, at);
          const auto n = std::min<std::uint64_t>(
              {16384, count - done,
               dense(l, v ? v->layout : nullptr, at, sizeof(T))});
          auto* target = static_cast<unsigned char*>(out.allocation) +
                         offset(l, out.byte_offset, at);
          if constexpr (kCopy)
            std::memcpy(target,
                        static_cast<const unsigned char*>(v->allocation) +
                            offset(*v->layout, v->byte_offset, at),
                        n * sizeof(T));
          else
            std::memset(target, 0, n * sizeof(T));
          done += n;
          advance(&at, l, l.rank, n);
        }
      } else {
        const auto load = [&](unsigned port, std::uint64_t c) {
          auto position = at;
          position[axis] = c;
          const auto raw = reader.raw(port, 0, position);
          T value;
          std::memcpy(&value, &raw, sizeof(T));
          return value;
        };
        const auto pixels = count / l.shape[axis];
        for (std::uint64_t done = 0; done < pixels;) {
          cancel(inv);
          std::uint64_t n = 1;
          const auto output_width = K == Kind::PlusRaw
                                        ? (out.port == 0 ? colors.size() : 1)
                                        : channels;
          bool fast = packed && axis > 0 && !l.origin[axis] &&
                      l.shape[axis] == output_width &&
                      l.strides[axis] == sizeof(T) &&
                      l.strides[axis - 1] ==
                          static_cast<std::int64_t>(output_width * sizeof(T));
          const T* ptr[3]{};
          auto* target = static_cast<unsigned char*>(out.allocation) +
                         offset(l, out.byte_offset, at);
          fast &= reinterpret_cast<std::uintptr_t>(target) % alignof(T) == 0;
          if (fast) {
            n = std::min<std::uint64_t>(
                4096, l.origin[axis - 1] + l.shape[axis - 1] - at[axis - 1]);
            for (unsigned port = 0; port < (K == Kind::BlendedOver ? 3U : 2U);
                 ++port) {
              auto position = at;
              position[axis] = port < 2 ? alpha : 0;
              const auto& v = reader.view(port, 0, position);
              const auto width = port == 2 ? colors.size() : channels;
              fast &= !v.layout->origin[axis] &&
                      v.layout->shape[axis] == width &&
                      v.layout->strides[axis] == sizeof(T) &&
                      v.layout->strides[axis - 1] ==
                          static_cast<std::int64_t>(width * sizeof(T));
              if (!fast)
                break;
              n = std::min(n, v.layout->origin[axis - 1] +
                                  v.layout->shape[axis - 1] - at[axis - 1]);
              position[axis] = 0;
              const auto* address =
                  static_cast<const unsigned char*>(v.allocation) +
                  offset(*v.layout, v.byte_offset, position);
              fast &=
                  reinterpret_cast<std::uintptr_t>(address) % alignof(T) == 0;
              ptr[port] = reinterpret_cast<const T*>(address);
            }
          }
          if (fast) {
            if constexpr (K == Kind::PlusRaw) {
              if (channels == 2)
                raw_span<T, 2>(ptr[0], ptr[1], reinterpret_cast<T*>(target), n,
                               out.port == 1);
              else
                raw_span<T, 4>(ptr[0], ptr[1], reinterpret_cast<T*>(target), n,
                               out.port == 1);
            } else if (channels == 2) {
              span<K, T, 2>(ptr[0], ptr[1], ptr[2],
                            reinterpret_cast<T*>(target), n);
            } else {
              span<K, T, 4>(ptr[0], ptr[1], ptr[2],
                            reinterpret_cast<T*>(target), n);
            }
          } else {
            n = 1;
            bool color_needed = K == Kind::PlusRaw && out.port == 0;
            if constexpr (K != Kind::PlusRaw)
              for (auto c : colors)
                color_needed |=
                    c >= l.origin[axis] && c - l.origin[axis] < l.shape[axis];
            bool selected = color_needed || K == Kind::PlusRaw ||
                            (alpha >= l.origin[axis] &&
                             alpha - l.origin[axis] < l.shape[axis]);
            Weights<T> w{};
            if (selected) {
              if constexpr (K == Kind::SourceAtop) {
                if (!color_needed)
                  w.alpha = load(1, alpha);
                else
                  w = weights<K>(load(0, alpha), load(1, alpha));
              } else if constexpr (K == Kind::DestinationAtop) {
                if (!color_needed)
                  w.alpha = load(0, alpha);
                else
                  w = weights<K>(load(0, alpha), load(1, alpha));
              } else {
                w = weights<K>(load(0, alpha), load(1, alpha));
              }
            }
            for (auto c = l.origin[axis]; c < l.origin[axis] + l.shape[axis];
                 ++c) {
              if ((c - l.origin[axis]) % 4096 == 0)
                cancel(inv);
              T value;
              std::uint64_t raw = 0;
              if constexpr (K == Kind::PlusRaw) {
                value = out.port == 1
                            ? w.alpha
                            : color<K>(
                                  w, [&] { return load(0, colors[c]); },
                                  [&] { return load(1, colors[c]); },
                                  [] { return T(0); });
                std::memcpy(&raw, &value, sizeof(T));
              } else {
                const auto found = std::find(colors.begin(), colors.end(), c);
                if (found == colors.end() && c != alpha) {
                  auto position = at;
                  position[axis] = c;
                  raw = reader.raw(1, 0, position);
                } else {
                  value = c == alpha
                              ? w.alpha
                              : color<K>(
                                    w, [&] { return load(0, c); },
                                    [&] { return load(1, c); },
                                    [&] {
                                      return load(2, found - colors.begin());
                                    });
                  std::memcpy(&raw, &value, sizeof(T));
                }
              }
              auto position = at;
              position[axis] = c;
              store(out, position, raw);
            }
          }
          done += n;
          advance(&at, l, axis, n);
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (type == ElementType::Float32)
      compute<float>(k, inv, work);
    else
      compute<double>(k, inv, work);
  }
};

template <Kind K>
OperationDefinition operation(const char* key, bool encoded = false) {
  using P = OperationParameterType;
  std::vector<OperationParameterSpec> p;
  if constexpr (!Coverage<K>::kPrimitive)
    p = {{"axis", P::Int64, false},
         {"components", P::String, false},
         {"alpha_index", P::Int64, false},
         {"input_description", P::String, false},
         {"group", P::String, false}};
  std::vector<std::pair<std::string, SchemaTemplate>> out;
  out.emplace_back(K == Kind::PlusRaw ? "color_mass" : "image",
                   tensor_schema(ElementType::Float64, {1}));
  if constexpr (K == Kind::PlusRaw)
    out.emplace_back("mass", tensor_schema(ElementType::Float64, {1}));
  return budgeted_operation(generation::operation(
      key,
      Coverage<K>::kPrimitive  ? 1
      : K == Kind::BlendedOver ? 3
                               : 2,
      std::move(p), out, [encoded](const auto& in, const auto& parameters) {
        return std::make_shared<Coverage<K>>(in, parameters, encoded);
      }));
}
}  // namespace ps::plugin_internal::coverage

namespace ps::plugin_internal {
Status register_coverage(OperationRegistry* registry) {
  using coverage::Kind;
  using coverage::operation;
  for (auto op :
       {operation<Kind::Clear>("composite.clear"),
        operation<Kind::Source>("composite.source"),
        operation<Kind::Destination>("composite.destination"),
#define PS_CMP(K, KEY)                            \
  operation<Kind::K>("composite." KEY "_linear"), \
      operation<Kind::K>("composite." KEY "_encoded", true)
        PS_CMP(SourceOver, "source_over"),
        PS_CMP(DestinationOver, "destination_over"),
        PS_CMP(BlendedOver, "source_over_blended_color"),
        PS_CMP(SourceIn, "source_in"), PS_CMP(DestinationIn, "destination_in"),
        PS_CMP(SourceOut, "source_out"),
        PS_CMP(DestinationOut, "destination_out"),
        PS_CMP(SourceAtop, "source_atop"),
        PS_CMP(DestinationAtop, "destination_atop"), PS_CMP(Xor, "xor"),
        PS_CMP(PlusRaw, "plus_raw"),
        PS_CMP(PlusBounded, "plus_lighter_bounded")}) {
#undef PS_CMP
    const auto status = registry->register_operation(std::move(op));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
