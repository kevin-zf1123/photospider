// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "09-composite/blend_composite_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::blend {
using namespace generation;  // NOLINT(build/namespaces)

inline bool region_empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return !d.extent; });
}
inline void checkpoint(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
inline void next_pixel(Coordinate* at, const ps_kernel_layout_v8& l,
                       unsigned component_axis, std::uint64_t n) {
  for (unsigned a = l.rank; a-- > 0 && n;) {
    if (a == component_axis)
      continue;
    n += (*at)[a] - l.origin[a];
    (*at)[a] = l.origin[a] + n % l.shape[a];
    n /= l.shape[a];
  }
}

struct Composite final : Program {
  CompositeKind kind;
  ElementType type;
  unsigned axis = 0, x_axis = 0, y_axis = 0;
  std::vector<std::uint64_t> colors;
  std::uint64_t alpha = 0, channels = 0;
  std::uint64_t seed_state = 0, frame = 0, origin_x = 0, origin_y = 0;
  bool packed = false;
  std::shared_ptr<const Luma> luma;

  Composite(CompositeKind k, const std::vector<OperationMetadata>& in,
            const Parameters& p)
      : kind(k) {
    const unsigned base = kind == CompositeKind::Dissolve ? 1 : 0;
    const auto& main = tensor(in.at(base), true);
    type = main.descriptor.element_type;
    const auto dims = main.sample_shape();
    require(floating(type), "BLN requires Float32/64");
    for (unsigned port = 0; port < in.size(); ++port) {
      const auto& t = tensor(in[port], true);
      require(t.descriptor.element_type == type, "BLN input dtype mismatch");
      if (kind == CompositeKind::Emission && port == 2) {
        require(t.batch_axes.empty() &&
                    t.descriptor.shape == std::vector<std::uint64_t>{1},
                "BLN gain must be a T[1] tensor");
      } else {
        require(t.batch_axes == main.batch_axes &&
                    t.descriptor.shape.size() == main.descriptor.shape.size(),
                "BLN input grid/batch mismatch");
      }
    }
    try {
      auto selection = color_point::select(in[base], p);
      const auto color_count = selection.channels.size();
      require(selection.axis >= 0 && (color_count == 1 || color_count == 3),
              "BLN requires an explicit Gray or three-color axis");
      if (whole_color(kind) || kind == CompositeKind::Emission)
        require(color_count == 3, "BLN member requires three coordinates");
      if (color_count == 3)
        selection = color_point::select(
            in[base], p,
            whole_color(kind) && !linear_y(kind)
                ? std::vector<std::string>{"l", "a", "b"}
                : std::vector<std::string>{"red", "green", "blue"});
      axis = static_cast<unsigned>(selection.axis);
      colors = selection.channels;
      channels = dims[axis];
      if (kind != CompositeKind::Emission) {
        const auto other = color_point::select(
            in[1 - base], p,
            color_count == 1 ? std::vector<std::string>{}
            : whole_color(kind) && !linear_y(kind)
                ? std::vector<std::string>{"l", "a", "b"}
                : std::vector<std::string>{"red", "green", "blue"});
        require(other.axis == selection.axis && other.channels == colors,
                "BLN image selectors disagree");
      }
      for (unsigned port = 0; port < in.size(); ++port) {
        if (kind == CompositeKind::Emission && port == 2)
          continue;
        const auto& other = tensor(in[port], true);
        if (other.layout.spatial) {
          require(
              other.layout.channel_axis &&
                  *other.layout.channel_axis + other.batch_axes.size() == axis,
              "BLN field component axis disagrees");
          require(!main.layout.spatial ||
                      (other.layout.height_axis == main.layout.height_axis &&
                       other.layout.width_axis == main.layout.width_axis),
                  "BLN spatial axes disagree");
        }
      }
      const auto& interpretation = selection.interpretation;
      const char* model = kind == CompositeKind::DarkerOklab ||
                                  kind == CompositeKind::LighterOklab
                              ? "oklab"
                          : kind == CompositeKind::DarkerLab ||
                                  kind == CompositeKind::LighterLab
                              ? "cielab"
                          : color_count == 1 ? "gray"
                                             : "rgb";
      require(!selection.semantic || interpretation.model == model,
              "BLN native color model mismatch");
      if (selection.semantic &&
          (linear_y(kind) || kind == CompositeKind::Emission ||
           kind == CompositeKind::MixLinear))
        require(interpretation.transfer == "linear",
                "BLN requires linear coordinates");
      if (selection.semantic && kind == CompositeKind::MixEncoded)
        require(!interpretation.transfer.empty() &&
                    interpretation.transfer != "linear",
                "BLN requires explicit encoded coordinates");
      if (whole_color(kind)) {
        require(channels == 3,
                "BLN whole-color ports contain exactly three coordinates");
      } else {
        std::optional<std::uint64_t> selected_alpha;
        if (selection.semantic) {
          require(!p.count("alpha_index"),
                  "BLN description forbids raw alpha_index");
          for (const auto& group : selection.description->groups)
            if (group.name == generation::text(p, "group"))
              selected_alpha = group.alpha;
          require(selected_alpha.has_value(), "BLN color group requires alpha");
          if (*selected_alpha < selection.description->channels.size())
            color_point::native_encoding(
                selection.description->channels[*selected_alpha].encoding);
        } else {
          require(p.count("alpha_index"), "BLN requires explicit alpha_index");
          const auto value = integer(p, "alpha_index", -1);
          require(value >= 0, "BLN alpha_index must be nonnegative");
          selected_alpha = static_cast<std::uint64_t>(value);
        }
        alpha = *selected_alpha;
        require(alpha < channels && std::find(colors.begin(), colors.end(),
                                              alpha) == colors.end(),
                "BLN alpha must be separate from color");
      }
      if (linear_y(kind)) {
        format::RgbBasis assertion;
        assertion.preset = interpretation.primaries;
        assertion.primaries_xy = interpretation.primaries_xy;
        assertion.white = interpretation.white;
        require(p.count("source_basis") || selection.semantic,
                "BLN linear Y requires explicit source_basis");
        const auto basis =
            p.count("source_basis")
                ? basis_ops::parse_basis(generation::text(p, "source_basis"))
                : basis_ops::resolve(assertion);
        require(interpretation.primaries.empty() ||
                    basis_ops::preset(interpretation.primaries).xy == basis.xy,
                "BLN basis assertion disagrees");
        require(!interpretation.primaries_xy ||
                    *interpretation.primaries_xy == basis.xy,
                "BLN primaries assertion disagrees");
        require(!interpretation.white || *interpretation.white == basis.white,
                "BLN white assertion disagrees");
        const auto matrix = basis_ops::rgb_matrix(basis.xy, basis.white);
        std::array<double, 3> raw;
        for (unsigned c = 0; c < 3; ++c) {
          const auto bits = matrix[3 + c].floating_bits(false);
          require(basis_ops::finite_bits(bits, false),
                  "BLN finite Y coefficients required");
          std::memcpy(&raw[c], &bits, 8);
        }
        luma = std::make_shared<const Luma>(raw);
        scratch_bytes = sizeof(MeasureWorkspace);
      }
      if (kind == CompositeKind::DarkerLab ||
          kind == CompositeKind::LighterLab) {
        require(p.count("source_white") || interpretation.white.has_value(),
                "BLN CIELAB requires explicit source_white");
        if (!p.count("source_white"))
          (void)basis_ops::white_xyz(*interpretation.white);
        if (p.count("source_white")) {
          const auto white =
              basis_ops::parse_white(generation::text(p, "source_white"));
          (void)basis_ops::white_xyz(white);
          require(!interpretation.white || *interpretation.white == white,
                  "BLN white assertion disagrees");
        }
      }
      if (kind == CompositeKind::Dissolve) {
        const auto domain = generation::text(
            p, "working_domain",
            selection.semantic
                ? (interpretation.transfer == "linear" ? "linear" : "encoded")
                : "");
        require(domain == "linear" || domain == "encoded",
                "BLN requires working_domain");
        require(
            !selection.semantic ||
                (!interpretation.transfer.empty() &&
                 (domain == "linear") == (interpretation.transfer == "linear")),
            "BLN working domain assertion disagrees");
        const auto resolve_axis = [&](const char* name, unsigned declared) {
          require(p.count(name) || main.layout.spatial,
                  "BLN explicit x/y axes required");
          const auto cell_axis = integer(p, name, declared);
          require(cell_axis >= 0 && static_cast<std::size_t>(cell_axis) <
                                        main.descriptor.shape.size(),
                  "BLN x/y axis outside cell rank");
          require(!main.layout.spatial ||
                      static_cast<unsigned>(cell_axis) == declared,
                  "BLN x/y axis disagrees with spatial layout");
          return static_cast<unsigned>(cell_axis + main.batch_axes.size());
        };
        x_axis = resolve_axis("x_axis", main.layout.width_axis);
        y_axis = resolve_axis("y_axis", main.layout.height_axis);
        require(x_axis != y_axis && x_axis != axis && y_axis != axis,
                "BLN x/y/color axes must be distinct");
        for (unsigned port = 0; port < 2; ++port) {
          const auto& image = tensor(in[port], true);
          require(!image.layout.spatial ||
                      (image.layout.width_axis + image.batch_axes.size() ==
                           x_axis &&
                       image.layout.height_axis + image.batch_axes.size() ==
                           y_axis),
                  "BLN dissolve spatial axes disagree");
        }
        const auto ox = integer(p, "origin_x"), oy = integer(p, "origin_y");
        require(static_cast<__int128>(ox) + dims[x_axis] - 1 <= INT64_MAX &&
                    static_cast<__int128>(oy) + dims[y_axis] - 1 <= INT64_MAX,
                "BLN coordinate exceeds Int64");
        origin_x = static_cast<std::uint64_t>(ox);
        origin_y = static_cast<std::uint64_t>(oy);
        seed_state =
            counter_mix(static_cast<std::uint64_t>(integer(p, "seed")));
        frame = static_cast<std::uint64_t>(integer(p, "frame"));
      }
    } catch (const basis_ops::CheckedFailure& e) {
      throw e.status;
    } catch (const basis_ops::GeometryError&) {
      throw Status{ErrorCode::InvalidArgument, "BLN invalid RGB basis/white"};
    }
    for (unsigned port = 0; port < in.size(); ++port) {
      if (kind == CompositeKind::Emission && port == 2)
        continue;
      auto expected = dims;
      if (kind == CompositeKind::Emission && port == 1)
        expected[axis] = 3;
      if ((kind == CompositeKind::MixLinear ||
           kind == CompositeKind::MixEncoded) &&
          port == 2)
        expected[axis] = 1;
      require(tensor(in[port], true).sample_shape() == expected,
              "BLN input shape mismatch");
    }
    packed = axis + 1 == dims.size();
    for (unsigned c = 0; c < colors.size(); ++c)
      packed &= colors[c] == c;
    packed &= whole_color(kind)
                  ? channels == 3
                  : alpha == colors.size() && channels == colors.size() + 1;
    if (kind == CompositeKind::Dissolve)
      packed &= x_axis + 1 == axis;
    if (kind == CompositeKind::Dissolve && packed)
      scratch_bytes = sizeof(DissolveWorkspace);
    auto output = *in[base].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    output.tensors[0].key = "samples";
    output.tensors[0].atomic_trailing_axes = 0;
    check(output.validate());
    add_output(this, std::move(output));
  }

  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (region_empty(r))
      return {};
    const unsigned base = kind == CompositeKind::Dissolve ? 1 : 0;
    std::vector<ReadRegion> result{{base, 0, r}};
    const auto includes = [&](std::uint64_t c) {
      const auto d = r.dimensions()[axis];
      return c >= d.offset && c - d.offset < d.extent;
    };
    const auto add = [&](unsigned port, std::uint64_t c) {
      auto d = r.dimensions();
      d[axis] = {c, 1};
      result.push_back({port, 0, Region(std::move(d))});
    };
    if (whole_color(kind)) {
      result.push_back({1, 0, r});
      for (unsigned port = 0; port < 2; ++port)
        for (unsigned c = 0; c < (linear_y(kind) ? 3U : 1U); ++c)
          add(port, colors[c]);
      return result;
    }
    bool needs_color = false;
    for (unsigned c = 0; c < colors.size(); ++c) {
      if (!includes(colors[c]))
        continue;
      needs_color = true;
      if (kind == CompositeKind::Emission)
        add(1, c);
      else
        add(kind == CompositeKind::Dissolve ? 0 : 1, colors[c]);
    }
    if (kind == CompositeKind::Emission) {
      if (needs_color)
        result.push_back({2, 0, Region({{0, 1}})});
    } else if (needs_color || includes(alpha)) {
      if (kind == CompositeKind::Dissolve) {
        add(0, alpha);
      } else {
        add(2, 0);
        add(1, alpha);
        if (needs_color)
          add(0, alpha);
      }
    }
    return result;
  }

  template <class T>
  void fast(const T* a, const T* b, const T* t, T gain, T* out, std::size_t n,
            const Coordinate& at, MeasureWorkspace* scratch,
            DissolveWorkspace* random_scratch, const Work& work) const {
    if (whole_color(kind)) {
      if (linear_y(kind)) {
        if (lighter(kind))
          whole_span<T, true, true>(a, b, out, n, luma.get(), scratch, work);
        else
          whole_span<T, true, false>(a, b, out, n, luma.get(), scratch, work);
      } else {
        if (lighter(kind))
          whole_span<T, false, true>(a, b, out, n, nullptr, nullptr, work);
        else
          whole_span<T, false, false>(a, b, out, n, nullptr, nullptr, work);
      }
    } else if (kind == CompositeKind::Emission) {
      emission_span(a, b, gain, out, n);
    } else if (kind == CompositeKind::Dissolve) {
      const auto* prefix =
          dissolve_prefix(random_scratch, seed_state, origin_x + at[x_axis], n);
      if (channels == 2)
        dissolve_span<T, 2>(a, b, out, n, seed_state, origin_x + at[x_axis],
                            origin_y + at[y_axis], frame, prefix);
      else
        dissolve_span<T, 4>(a, b, out, n, seed_state, origin_x + at[x_axis],
                            origin_y + at[y_axis], frame, prefix);
    } else {
      if (channels == 2)
        mix_span<T, 2>(a, b, t, out, n);
      else
        mix_span<T, 4>(a, b, t, out, n);
    }
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    DissolveWorkspace* random_scratch = nullptr;
    if (kind == CompositeKind::Dissolve && packed)
      random_scratch = new (inv.scratch[0].allocation) DissolveWorkspace;
    MeasureWorkspace* scratch = nullptr;
    if (luma)
      scratch = new (inv.scratch[0].allocation) MeasureWorkspace;
    struct Destroy {
      MeasureWorkspace* p;
      DissolveWorkspace* random;
      ~Destroy() {
        if (p)
          p->~MeasureWorkspace();
        if (random)
          random->~DissolveWorkspace();
      }
    } destroy{scratch, random_scratch};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& layout = *out.layout;
      Coordinate at{};
      std::uint64_t pixels = 1;
      for (unsigned a = 0; a < layout.rank; ++a) {
        at[a] = layout.origin[a];
        if (a != axis)
          pixels *= layout.shape[a];
      }
      if (!layout.shape[axis] || !pixels)
        continue;
      work.poll(pixels * layout.shape[axis] * 48);
      const auto load = [&](unsigned port, std::uint64_t c) {
        auto position = at;
        position[axis] = c;
        const auto raw = reader.raw(port, 0, position);
        T value;
        std::memcpy(&value, &raw, sizeof(T));
        return value;
      };
      const auto gain = [&]() {
        const auto raw = reader.raw(2, 0, Coordinate{});
        T value;
        std::memcpy(&value, &raw, sizeof(T));
        return value;
      };
      for (std::uint64_t done = 0; done < pixels;) {
        checkpoint(inv);
        std::uint64_t n = 1;
        bool continuous = packed && axis > 0 && !layout.origin[axis] &&
                          layout.shape[axis] == channels &&
                          layout.strides[axis] == sizeof(T) &&
                          layout.strides[axis - 1] ==
                              static_cast<std::int64_t>(channels * sizeof(T));
        const T* ptr[3]{};
        T* target = nullptr;
        T g{};
        if (continuous) {
          n = std::min<std::uint64_t>(
              4096,
              layout.origin[axis - 1] + layout.shape[axis - 1] - at[axis - 1]);
          const unsigned ports = kind == CompositeKind::MixLinear ||
                                         kind == CompositeKind::MixEncoded
                                     ? 3
                                     : 2;
          for (unsigned port = 0; port < ports; ++port) {
            auto position = at;
            position[axis] = 0;
            const auto& v = reader.view(port, 0, position);
            const auto width = kind == CompositeKind::Emission && port == 1 ? 3
                               : port == 2                                  ? 1
                                           : channels;
            continuous &= !v.layout->origin[axis] &&
                          v.layout->shape[axis] == width &&
                          v.layout->strides[axis] == sizeof(T) &&
                          v.layout->strides[axis - 1] ==
                              static_cast<std::int64_t>(width * sizeof(T));
            n = std::min(n, v.layout->origin[axis - 1] +
                                v.layout->shape[axis - 1] - at[axis - 1]);
            const auto* address =
                static_cast<const unsigned char*>(v.allocation) +
                offset(*v.layout, v.byte_offset, position);
            continuous &=
                reinterpret_cast<std::uintptr_t>(address) % alignof(T) == 0;
            ptr[port] = reinterpret_cast<const T*>(address);
          }
          auto position = at;
          position[axis] = 0;
          auto* address = static_cast<unsigned char*>(out.allocation) +
                          offset(layout, out.byte_offset, position);
          continuous &=
              reinterpret_cast<std::uintptr_t>(address) % alignof(T) == 0;
          target = reinterpret_cast<T*>(address);
          if (kind == CompositeKind::Emission)
            g = gain();
        }
        if (continuous) {
          fast(ptr[0], ptr[1], ptr[2], g, target, n, at, scratch,
               random_scratch, work);
        } else {
          n = 1;
          bool take_b = false;
          if (whole_color(kind)) {
            if (linear_y(kind)) {
              T b[3], s[3];
              for (unsigned c = 0; c < 3; ++c) {
                b[c] = load(0, colors[c]);
                s[c] = load(1, colors[c]);
              }
              take_b = luma->background(b, s, lighter(kind), scratch, work);
            } else {
              const T b = load(0, colors[0]), s = load(1, colors[0]);
              take_b = lighter(kind) ? b >= s : b <= s;
            }
          }
          const auto start = layout.origin[axis],
                     end = start + layout.shape[axis];
          for (auto c = start; c < end; ++c) {
            const auto found = std::find(colors.begin(), colors.end(), c);
            const bool color = found != colors.end();
            T value;
            if (whole_color(kind)) {
              value = load(take_b ? 0 : 1, c);
            } else if (!color && c != alpha) {
              value = load(kind == CompositeKind::Dissolve ? 1 : 0, c);
            } else if (kind == CompositeKind::Dissolve) {
              const T coverage = load(0, alpha);
              const bool hit =
                  coverage_hit(counter(seed_state, origin_x + at[x_axis],
                                       origin_y + at[y_axis], frame),
                               coverage);
              value = hit ? (c == alpha ? T(1) : load(0, c)) : load(1, c);
            } else if (kind == CompositeKind::Emission) {
              if (!color) {
                value = load(0, c);
              } else {
                const T gvalue = gain();
                value =
                    gvalue == 0
                        ? load(0, c)
                        : load(0, c) + gvalue * load(1, found - colors.begin());
              }
            } else {
              const T t = load(2, 0);
              if (t == 0 || t == 1) {
                value = load(t == 0 ? 0 : 1, c);
              } else {
                const T a = (T(1) - t) * load(0, alpha), b = t * load(1, alpha),
                        mass = a + b;
                value = c == alpha ? mass
                        : mass > 0 ? ((a == 0 ? T(0) : a * load(0, c)) +
                                      (b == 0 ? T(0) : b * load(1, c))) /
                                         mass
                                   : T(0);
              }
            }
            auto position = at;
            position[axis] = c;
            std::uint64_t raw = 0;
            std::memcpy(&raw, &value, sizeof(T));
            generation::store(out, position, raw);
          }
        }
        done += n;
        next_pixel(&at, layout, axis, n);
      }
    }
    checkpoint(inv);
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

OperationDefinition composite_operation(CompositeKind kind, const char* key) {
  using P = OperationParameterType;
  std::vector<OperationParameterSpec> p{{"axis", P::Int64, false},
                                        {"components", P::String, false},
                                        {"input_description", P::String, false},
                                        {"group", P::String, false}};
  if (linear_y(kind))
    p.push_back({"source_basis", P::String, false});
  if (kind == CompositeKind::DarkerLab || kind == CompositeKind::LighterLab)
    p.push_back({"source_white", P::String, false});
  if (!whole_color(kind))
    p.push_back({"alpha_index", P::Int64, false});
  if (kind == CompositeKind::Dissolve)
    append(&p, {{"working_domain", P::String, false},
                {"x_axis", P::Int64, false},
                {"y_axis", P::Int64, false},
                {"origin_x", P::Int64, false},
                {"origin_y", P::Int64, false},
                {"seed", P::Int64, false},
                {"frame", P::Int64, false}});
  const unsigned count = kind == CompositeKind::Emission ||
                                 kind == CompositeKind::MixLinear ||
                                 kind == CompositeKind::MixEncoded
                             ? 3
                             : 2;
  return budgeted_operation(generation::operation(
      key, count, std::move(p),
      {{whole_color(kind) ? "color" : "image",
        tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& inputs, const auto& parameters) {
        return std::make_shared<Composite>(kind, inputs, parameters);
      }));
}
}  // namespace ps::plugin_internal::blend

namespace ps::plugin_internal {
Status register_blend_composite(OperationRegistry* registry) {
  using blend::composite_operation;
  using blend::CompositeKind;
  for (auto op : {composite_operation(CompositeKind::DarkerY,
                                      "blend.darker_color_linear_y"),
                  composite_operation(CompositeKind::LighterY,
                                      "blend.lighter_color_linear_y"),
                  composite_operation(CompositeKind::DarkerOklab,
                                      "blend.darker_color_oklab_l"),
                  composite_operation(CompositeKind::LighterOklab,
                                      "blend.lighter_color_oklab_l"),
                  composite_operation(CompositeKind::DarkerLab,
                                      "blend.darker_color_cielab_l"),
                  composite_operation(CompositeKind::LighterLab,
                                      "blend.lighter_color_cielab_l"),
                  composite_operation(CompositeKind::Dissolve,
                                      "blend.dissolve_counter64"),
                  composite_operation(CompositeKind::Emission,
                                      "blend.emission_add_straight")}) {
    const auto status = registry->register_operation(std::move(op));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
