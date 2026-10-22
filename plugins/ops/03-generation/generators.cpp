#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/math.hpp"
#include "common/registry.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal::generation {
std::uint64_t ellipse_coverage(Math&, const std::array<Rational, 2>&,
                               const std::array<Rational, 2>&,
                               const std::array<Rational, 2>&, bool);
std::uint64_t ellipse_distance(Math&, const std::array<Rational, 2>&,
                               const std::array<Rational, 2>&, bool);
namespace {
enum class Kind {
  Constant,
  Color,
  Coordinates,
  Axis,
  Rectangle,
  Ellipse,
  RectangleSdf,
  EllipseSdf,
  Checker,
  Grid,
  Ramp,
  Impulse,
  Zone,
  Siemens,
  Bars
};
struct Generator final : Program, Canvas {
  Kind kind;
  unsigned axis = 1;
  std::uint64_t refinements = 4096;
  bool normalized = false;
  std::int64_t cycles = 1;
  double lo = 0, hi = 1, amplitude = 1, background = 0;
  std::uint64_t channels = 1;
  explicit Generator(Kind selected, const Parameters& p)
      : Canvas(p), kind(selected) {
    const auto named_axis = text(p, "axis", "x");
    require(named_axis == "x" || named_axis == "y",
            "generation axis must be x or y");
    axis = named_axis == "x" ? 1 : 0;
    const auto space = text(p, "space", "pixel");
    require(space == "pixel" || space == "normalized_edge",
            "generation coordinate space");
    normalized = space == "normalized_edge";
    cycles = integer(p, "cycles", 1);
    lo = real(p, "lo", 0);
    hi = real(p, "hi", 1);
    amplitude = real(p, "amplitude", 1);
    background = real(p, "background", 0);
    refinements =
        static_cast<std::uint64_t>(integer(p, "max_refinements", 4096));
    scratch_bytes = kind == Kind::Constant || kind == Kind::Color ||
                            kind == Kind::Impulse || kind == Kind::Bars
                        ? 0
                        : sizeof(Math);
  }
  std::vector<ReadRegion> support(unsigned out,
                                  const Region& region) const override {
    if (kind != Kind::Constant && kind != Kind::Color) {
      return Program::support(out, region);
    }
    Region selected({region.dimensions()[2]});
    return {{0, 0, std::move(selected)}};
  }
  void fill(const Kernel& kernel,
            const ps_kernel_invocation_v8& invocation) const {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    const auto& out = invocation.outputs[0];
    const auto& layout = *out.layout;
    const auto bytes = generation::width(type);
    for (std::uint64_t c = layout.origin[2];
         c < layout.origin[2] + layout.shape[2]; ++c) {
      const auto value = reader.bits(0, 0, {c});
      for (std::uint64_t y = layout.origin[0];
           y < layout.origin[0] + layout.shape[0]; ++y) {
        work.poll(layout.shape[1]);
        auto* row = static_cast<std::uint8_t*>(out.allocation) +
                    offset(layout, out.byte_offset, {y, layout.origin[1], c});
        if (layout.strides[1] == static_cast<std::int64_t>(bytes) &&
            reinterpret_cast<std::uintptr_t>(row) % bytes == 0) {
          switch (bytes) {
            case 1:
              std::memset(row, static_cast<unsigned char>(value),
                          layout.shape[1]);
              break;
            case 2:
              std::fill_n(reinterpret_cast<std::uint16_t*>(row),
                          layout.shape[1], static_cast<std::uint16_t>(value));
              break;
            case 4:
              std::fill_n(reinterpret_cast<std::uint32_t*>(row),
                          layout.shape[1], static_cast<std::uint32_t>(value));
              break;
            default:
              std::fill_n(reinterpret_cast<std::uint64_t*>(row),
                          layout.shape[1], value);
              break;
          }
        } else {
          for (std::uint64_t x = 0; x < layout.shape[1]; ++x) {
            store(out, {y, x + layout.origin[1], c}, value);
          }
        }
      }
    }
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    if (kind == Kind::Constant || kind == Kind::Color) {
      fill(kernel, invocation);
      return;
    }
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    const bool narrow = type == ElementType::Float32;
    const auto& out = invocation.outputs[0];
    if (kind == Kind::Impulse || kind == Kind::Bars) {
      std::int64_t ix = 0, iy = 0;
      if (kind == Kind::Impulse) {
        ix = reader.index(0, 0, {0});
        iy = reader.index(0, 0, {1});
      }
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        std::uint64_t result;
        if (kind == Kind::Impulse) {
          result = convert(at[1] == static_cast<std::uint64_t>(ix) &&
                                   at[0] == static_cast<std::uint64_t>(iy)
                               ? amplitude
                               : background,
                           narrow);
        } else {
          const auto bar = static_cast<unsigned>((4 * (2 * at[1] + 1)) / width);
          constexpr unsigned masks[8] = {7, 6, 3, 2, 5, 4, 1, 0};
          result = convert((masks[bar] >> (2 - at[2])) & 1, narrow);
        }
        store(out, at, result);
      });
      return;
    }
    MathArena arena(invocation.scratch[0].allocation, work, refinements);
    auto& m = *arena;
    auto& e = m.exact;
    std::array<Rational, 2> a{}, b{}, c{};
    Rational k, phase;
    const auto pair = [&](unsigned port) {
      return std::array<Rational, 2>{m.input(reader, port, 0, {0}),
                                     m.input(reader, port, 0, {1})};
    };
    if (kind == Kind::Rectangle) {
      a = pair(0);
      b = {m.input(reader, 0, 0, {2}), m.input(reader, 0, 0, {3})};
    } else if (kind == Kind::Ellipse || kind == Kind::RectangleSdf ||
               kind == Kind::EllipseSdf || kind == Kind::Checker ||
               kind == Kind::Grid) {
      a = pair(0);
      b = pair(1);
      if (kind == Kind::Grid) {
        c = pair(2);
      }
    } else if (kind == Kind::Zone || kind == Kind::Siemens) {
      a = pair(0);
      if (kind == Kind::Zone) {
        k = m.input(reader, 1, 0, {0});
      }
      phase = m.input(reader, kind == Kind::Zone ? 2 : 1, 0, {0});
    }
    const auto low = m.value(lo), high = m.value(hi),
               difference = m.sub(high, low);
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      e.reset();
      std::uint64_t result = 0;
      const auto x = m.center(origin_x, at[1]), y = m.center(origin_y, at[0]);
      if (kind == Kind::Coordinates || kind == Kind::Axis) {
        const auto selected =
            kind == Kind::Coordinates ? (at[2] == 0 ? 1U : 0U) : axis;
        auto value = selected ? x : y;
        if (normalized) {
          value = m.div(m.add(e.integer(at[selected]), e.fraction(1, 2)),
                        e.integer(selected ? width : height));
        }
        result = m.rounded(value, narrow);
      } else if (kind == Kind::Rectangle) {
        const auto px = m.sub(x, e.fraction(1, 2)),
                   py = m.sub(y, e.fraction(1, 2));
        const auto dx = m.positive(
            m.sub(m.min(b[0], m.add(px, e.integer(1))), m.max(a[0], px)));
        const auto dy = m.positive(
            m.sub(m.min(b[1], m.add(py, e.integer(1))), m.max(a[1], py)));
        result = m.rounded(m.mul(dx, dy), narrow);
      } else if (kind == Kind::Ellipse) {
        result = ellipse_coverage(
            m, {m.sub(x, e.fraction(1, 2)), m.sub(y, e.fraction(1, 2))}, a, b,
            narrow);
      } else if (kind == Kind::RectangleSdf || kind == Kind::EllipseSdf) {
        const std::array<Rational, 2> delta{m.sub(x, a[0]), m.sub(y, a[1])};
        if (kind == Kind::EllipseSdf) {
          result = ellipse_distance(m, delta, b, narrow);
        } else {
          const auto qx = m.sub(e.absolute(delta[0]), b[0]),
                     qy = m.sub(e.absolute(delta[1]), b[1]);
          const auto inside = m.min(m.max(qx, qy), e.integer(0));
          result = m.affine_sqrt(
              inside, e.integer(1),
              m.add(m.sq(m.positive(qx)), m.sq(m.positive(qy))), narrow);
        }
      } else if (kind == Kind::Checker || kind == Kind::Grid) {
        const auto u = m.div(m.sub(x, b[0]), a[0]),
                   v = m.div(m.sub(y, b[1]), a[1]);
        bool chosen;
        if (kind == Kind::Checker) {
          chosen =
              ((e.small_floor(e.modulo(u, 2)) + e.small_floor(e.modulo(v, 2))) &
               1) != 0;
        } else {
          chosen = e.order(m.mul(a[0], e.modulo(u, 1)), c[0]) < 0 ||
                   e.order(m.mul(a[1], e.modulo(v, 1)), c[1]) < 0;
        }
        result = convert(chosen ? hi : lo, narrow);
      } else if (kind == Kind::Ramp) {
        const auto extent = axis ? width : height, index = at[axis];
        if (extent == 1 || index == 0) {
          result = convert(lo, narrow);
        } else if (index == extent - 1) {
          result = convert(hi, narrow);
        } else {
          result = m.rounded(m.div(m.add(m.scale(low, extent - 1 - index),
                                         m.scale(high, index)),
                                   e.integer(extent - 1)),
                             narrow);
        }
      } else {
        const auto dx = m.sub(x, a[0]), dy = m.sub(y, a[1]);
        const auto radius2 = m.add(m.sq(dx), m.sq(dy));
        int octant = -1;
        if (Exact::zero(phase)) {
          if (kind == Kind::Zone) {
            const auto quarters = e.modulo(m.scale(m.mul(k, radius2), 4), 4);
            const auto q = e.small_floor(quarters);
            if (e.order(quarters, e.integer(q)) == 0) {
              octant = 2 * static_cast<int>(q);
            }
          } else if (!cycles) {
            octant = 0;
          } else if (Exact::zero(dy)) {
            octant = dx.negative ? 4 : 0;
          } else if (Exact::zero(dx)) {
            octant = dy.negative ? 6 : 2;
          } else if (e.order(e.absolute(dx), e.absolute(dy)) == 0) {
            octant =
                dx.negative ? (dy.negative ? 5 : 3) : (dy.negative ? 7 : 1);
          }
          if (kind == Kind::Siemens && octant >= 0) {
            octant = static_cast<int>((static_cast<std::uint64_t>(cycles) *
                                       static_cast<unsigned>(octant)) &
                                      7);
          }
        }
        if (kind == Kind::Siemens && Exact::zero(dx) && Exact::zero(dy)) {
          result = m.rounded(m.div(m.add(low, high), e.integer(2)), narrow);
        } else if (Exact::zero(difference)) {
          result = m.rounded(low, narrow);
        } else if (octant >= 0) {
          const auto midpoint = m.div(m.add(low, high), e.integer(2));
          if (!(octant & 1)) {
            result = m.rounded(octant == 0   ? high
                               : octant == 4 ? low
                                             : midpoint,
                               narrow);
          } else {
            result = m.affine_sqrt(
                midpoint,
                m.div((octant == 1 || octant == 7) ? difference
                                                   : e.negate(difference),
                      e.integer(2)),
                e.fraction(1, 2), narrow);
          }
        } else {
          result = m.certify(narrow, [&](Interval value) {
            auto angle = m.d.interval(), temp = m.d.interval(),
                 sine = m.d.interval(), cosine = m.d.interval();
            if (kind == Kind::Zone) {
              m.d.constant(angle, true);
              m.d.scale(angle, angle, 1);
              m.enclose(temp, m.mul(k, radius2));
              m.d.multiply(angle, angle, temp);
            } else {
              m.atan2(angle, dy, dx);
              m.d.integer(temp, cycles);
              m.d.multiply(angle, angle, temp);
            }
            m.enclose(temp, phase);
            m.d.add(angle, angle, temp);
            m.sincos(sine, cosine, angle);
            m.d.integer(temp, 1);
            m.d.add(cosine, cosine, temp);
            m.d.scale(cosine, cosine, -1);
            m.enclose(temp, difference);
            m.d.multiply(cosine, cosine, temp);
            m.enclose(temp, low);
            m.d.add(value, temp, cosine);
          });
        }
      }
      check(e.status());
      store(out, at, result);
    });
  }
};
std::shared_ptr<Program> make(Kind kind,
                              const std::vector<OperationMetadata>& inputs,
                              const Parameters& p) {
  auto state = std::make_shared<Generator>(kind, p);
  const auto validate = [&](unsigned port, std::vector<std::uint64_t> expected,
                            bool floats = true) {
    shape(inputs[port], expected, floats);
  };
  switch (kind) {
    case Kind::Constant:
    case Kind::Color: {
      const auto& field = tensor(inputs[0]);
      require(field.sample_shape().size() == 1,
              "constant requires a component vector");
      state->channels = field.sample_shape()[0];
      state->type = field.descriptor.element_type;
      if (kind == Kind::Color) {
        require(floating(state->type),
                "constant image requires floating color");
        auto description = take(
            tensor_description_from_parameter(text(p, "input_description")));
        check(validate_tensor_description(description, field.descriptor));
        require(description.channel_axis && *description.channel_axis == 0 &&
                    !description.groups.empty(),
                "constant image requires complete explicit color groups");
        const auto selected = text(p, "color_group");
        const auto group = std::find_if(
            description.groups.begin(), description.groups.end(),
            [&](const auto& value) { return value.name == selected; });
        require(group != description.groups.end() && !group->indices.empty() &&
                    group->components.size() == group->indices.size() &&
                    group->interpretation.association == "straight",
                "constant image requires a selected straight color group");
      }
      break;
    }
    case Kind::Rectangle:
      validate(0, {4});
      break;
    case Kind::Ellipse:
    case Kind::RectangleSdf:
    case Kind::EllipseSdf:
    case Kind::Checker:
      validate(0, {2});
      validate(1, {2});
      break;
    case Kind::Grid:
      validate(0, {2});
      validate(1, {2});
      validate(2, {2});
      break;
    case Kind::Impulse:
      validate(0, {2}, false);
      require(tensor(inputs[0]).descriptor.element_type == ElementType::Int64,
              "impulse index requires Int64");
      break;
    case Kind::Zone:
      validate(0, {2});
      validate(1, {1});
      validate(2, {1});
      break;
    case Kind::Siemens:
      validate(0, {2});
      validate(1, {1});
      break;
    default:
      break;
  }
  if (kind == Kind::Coordinates) {
    state->channels = 2;
  }
  if (kind == Kind::Bars) {
    state->channels = 3;
  }
  const bool components = kind == Kind::Constant || kind == Kind::Color ||
                          kind == Kind::Coordinates || kind == Kind::Bars;
  auto dimensions = std::vector<std::uint64_t>{state->height, state->width};
  if (components) {
    dimensions.push_back(state->channels);
  }
  add_output(state.get(),
             tensor_schema(state->type, dimensions, true, components));
  return state;
}
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_generation(OperationRegistry* registry) {
  using generation::canvas_parameters;
  using generation::check;
  using generation::Kind;
  using generation::make;
  using generation::operation;
  using generation::tensor_schema;
  try {
    struct Entry {
      const char* name;
      Kind kind;
      unsigned inputs;
      const char* output;
      bool accelerated;
    };
    const Entry entries[] = {
        {"constant_tensor", Kind::Constant, 1, "values", true},
        {"constant_image", Kind::Color, 1, "image", true},
        {"coordinates_xy", Kind::Coordinates, 0, "coordinates", true},
        {"coordinate_axis", Kind::Axis, 0, "values", true},
        {"rectangle_coverage", Kind::Rectangle, 1, "coverage", true},
        {"ellipse_coverage", Kind::Ellipse, 2, "coverage", true},
        {"rectangle_sdf", Kind::RectangleSdf, 2, "distance", true},
        {"ellipse_sdf", Kind::EllipseSdf, 2, "distance", true},
        {"checker_pattern", Kind::Checker, 2, "values", false},
        {"grid_pattern", Kind::Grid, 3, "values", false},
        {"ramp_pattern", Kind::Ramp, 0, "values", true},
        {"impulse_pattern", Kind::Impulse, 1, "values", false},
        {"zone_plate", Kind::Zone, 3, "values", true},
        {"siemens_star", Kind::Siemens, 2, "values", true},
        {"linear_rgb_bars", Kind::Bars, 0, "image", false}};
    for (const auto& entry : entries) {
      auto parameters = canvas_parameters(entry.kind != Kind::Constant &&
                                          entry.kind != Kind::Color);
      const auto add = [&](const char* key, OperationParameterType type,
                           bool required = false) {
        parameters.push_back({key, type, required});
      };
      if (entry.kind == Kind::Color) {
        add("input_description", OperationParameterType::String, true);
        add("color_group", OperationParameterType::String, true);
      }
      if (entry.kind == Kind::Coordinates || entry.kind == Kind::Axis) {
        add("space", OperationParameterType::String);
      }
      if (entry.kind == Kind::Axis || entry.kind == Kind::Ramp) {
        add("axis", OperationParameterType::String);
      }
      if (entry.kind == Kind::Ellipse || entry.kind == Kind::EllipseSdf) {
        add("max_refinements", OperationParameterType::Int64);
      }
      if (entry.kind == Kind::Siemens) {
        add("cycles", OperationParameterType::Int64);
      }
      if (entry.kind == Kind::Impulse) {
        add("amplitude", OperationParameterType::Float64);
        add("background", OperationParameterType::Float64);
      }
      if (entry.kind == Kind::Checker || entry.kind == Kind::Grid ||
          entry.kind == Kind::Ramp || entry.kind == Kind::Zone ||
          entry.kind == Kind::Siemens) {
        add("lo", OperationParameterType::Float64);
        add("hi", OperationParameterType::Float64);
      }
      const auto prototype = tensor_schema(ElementType::Float64, {1, 1});
      for (unsigned profile = 0; profile < (entry.accelerated ? 3U : 1U);
           ++profile) {
        const char* suffix[] = {"strict", "accelerated_apple_silicon",
                                "accelerated_x86_64"};
        check(registry->register_operation(operation(
            std::string("generation.") + entry.name + "_v1_" + suffix[profile],
            entry.inputs, parameters, {{entry.output, prototype}},
            [kind = entry.kind, profile](const auto& inputs, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return make(kind, inputs, p);
            })));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
