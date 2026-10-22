#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/math.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class GradientKind {
  Linear,
  Radial,
  Angular,
  Diamond,
  Box,
  Spread,
  Lookup
};

struct Gradient final : Program {
  GradientKind kind;
  ElementType type;
  bool accelerated;
  std::uint64_t height = 1, width = 1, rows = 0, channels = 0;
  std::int64_t origin_x = 0, origin_y = 0;
  std::string method;
  std::uint64_t refinements;
  Gradient(GradientKind selected, const std::vector<OperationMetadata>& in,
           const Parameters& p, bool fast)
      : kind(selected),
        type(dtype(p)),
        accelerated(fast),
        method(text(p, "method", "pad")),
        refinements(
            static_cast<std::uint64_t>(integer(p, "max_refinements", 4096))) {
    scratch_bytes =
        kind == GradientKind::Angular ? sizeof(Math) : sizeof(ScalarMath<384>);
    if (kind == GradientKind::Spread) {
      const auto& input = tensor(in[0], true);
      require(floating(input.descriptor.element_type),
              "spread requires Float32/64");
      require(method == "pad" || method == "repeat" || method == "reflect",
              "unknown spread method");
      type = input.descriptor.element_type;
      scratch_bytes = 0;
      add_output(this, tensor_schema(type, input.sample_shape()));
    } else if (kind == GradientKind::Lookup) {
      scratch_bytes = sizeof(ScalarMath<128>);
      const auto& query = tensor(in[0], true);
      const auto& table = tensor(in[1], true);
      auto dimensions = query.sample_shape();
      const auto control = table.sample_shape();
      require(floating(query.descriptor.element_type) &&
                  floating(table.descriptor.element_type) &&
                  dimensions.size() < 8 && control.size() == 2 &&
                  control[0] >= 2,
              "gradient lookup requires floating S and table[N>=2,C]");
      rows = control[0];
      channels = control[1];
      dimensions.push_back(channels);
      add_output(this, tensor_schema(type, dimensions, false, true));
    } else {
      Canvas canvas(p);
      height = canvas.height;
      width = canvas.width;
      origin_x = canvas.origin_x;
      origin_y = canvas.origin_y;
      shape(in[0], {2});
      shape(in[1], {kind == GradientKind::Angular ? 1U : 2U});
      if (kind == GradientKind::Angular) {
        require(text(p, "center_policy", "zero") == "zero",
                "angular center_policy must be zero");
      }
      add_output(this, tensor_schema(type, {height, width}, true));
    }
  }
  std::vector<ReadRegion> support(unsigned output,
                                  const Region& region) const override {
    if (kind == GradientKind::Spread) {
      return {{0, 0, region}};
    }
    if (kind == GradientKind::Lookup) {
      auto query = region.dimensions();
      const auto components = query.back();
      query.pop_back();
      return {{0, 0, Region(std::move(query))},
              {1, 0, Region({{0, rows}, components})}};
    }
    return Program::support(output, region);
  }
  template <unsigned Words>
  void run_scalar(const Kernel& kernel,
                  const ps_kernel_invocation_v8& invocation,
                  const Work& work) const {
    using M = ScalarMath<Words>;
    using E = typename M::E;
    using R = typename M::R;
    auto* memory = new (invocation.scratch[0].allocation) M(work);
    struct Destroy {
      M* p;
      ~Destroy() { p->~M(); }
    } destroy{memory};
    auto& m = *memory;
    auto& e = m.exact;
    const Reader reader{kernel, invocation};
    const auto& out = invocation.outputs[0];
    const bool narrow = type == ElementType::Float32;
    if (kind == GradientKind::Lookup) {
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        e.reset();
        auto query = at;
        query[out.layout->rank - 1] = 0;
        const auto component = at[out.layout->rank - 1];
        const auto t = reader.number(0, 0, query);
        if (t == 0 || t == 1) {
          store(out, at,
                convert(reader.number(1, 0, {t == 0 ? 0 : rows - 1, component}),
                        narrow));
          return;
        }
        if (narrow || accelerated) {
          work.poll(16);
          const auto scaled =
              Fast::point(t) * Fast::point(static_cast<double>(rows - 1));
          const auto j = std::floor(scaled.low);
          if (scaled.finite() && j >= 0 && j < rows - 1 &&
              j == std::floor(scaled.high)) {
            const auto row = static_cast<std::uint64_t>(j);
            const auto w = scaled - Fast::point(j);
            const auto a = Fast::point(reader.number(1, 0, {row, component}));
            const auto b =
                Fast::point(reader.number(1, 0, {row + 1, component}));
            if (const auto value = fast_result((Fast::point(1) - w) * a + w * b,
                                               narrow, accelerated)) {
              store(out, at, *value);
              return;
            }
          }
        }
        const auto x = m.mul(m.input(reader, 0, query), e.integer(rows - 1));
        auto row = static_cast<std::uint64_t>(number(m.rounded(x, false)));
        // RN64 can cross at most one integer boundary for x<=2^40.
        if (e.order(x, e.integer(row)) < 0)
          --row;
        const auto w = m.sub(x, e.integer(row));
        const auto a = m.input(reader, 1, {row, component});
        if (E::zero(w)) {
          store(out, at, m.rounded(a, narrow, true));
        } else {
          const auto b = m.input(reader, 1, {row + 1, component});
          store(out, at,
                m.rounded(m.add(m.mul(m.sub(e.integer(1), w), a), m.mul(w, b)),
                          narrow));
        }
      });
      return;
    }
    const std::array<R, 2> a{m.input(reader, 0, {0}), m.input(reader, 0, {1})};
    std::array<R, 2> b{m.input(reader, 1, {0}), m.input(reader, 1, {1})};
    const std::array<Fast, 2> fa{Fast::point(reader.number(0, 0, {0})),
                                 Fast::point(reader.number(0, 0, {1}))};
    std::array<Fast, 2> fb{Fast::point(reader.number(1, 0, {0})),
                           Fast::point(reader.number(1, 0, {1}))};
    if (kind == GradientKind::Linear) {
      b = {m.sub(b[0], a[0]), m.sub(b[1], a[1])};
      fb = {fb[0] - fa[0], fb[1] - fa[1]};
    }
    const auto denominator = kind == GradientKind::Linear
                                 ? m.add(m.sq(b[0]), m.sq(b[1]))
                                 : e.integer(1);
    const auto fast_den = fb[0] * fb[0] + fb[1] * fb[1];
    check(e.status());
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      e.reset();
      if (narrow || accelerated) {
        work.poll(24);
        const auto dx = fast_center(origin_x, at[1]) - fa[0];
        const auto dy = fast_center(origin_y, at[0]) - fa[1];
        Fast value;
        if (kind == GradientKind::Linear) {
          value = (dx * fb[0] + dy * fb[1]) / fast_den;
        } else {
          const auto x = fast_abs(dx) / fb[0], y = fast_abs(dy) / fb[1];
          value = kind == GradientKind::Radial ? fast_sqrt(x * x + y * y)
                  : kind == GradientKind::Diamond
                      ? x + y
                      : Fast{std::max(x.low, y.low), std::max(x.high, y.high)};
        }
        if (const auto accepted = fast_result(value, narrow, accelerated)) {
          store(out, at, *accepted);
          return;
        }
      }
      const auto dx = m.sub(m.center(origin_x, at[1]), a[0]);
      const auto dy = m.sub(m.center(origin_y, at[0]), a[1]);
      std::uint64_t result;
      if (kind == GradientKind::Linear) {
        result = m.rounded(
            m.div(m.add(m.mul(dx, b[0]), m.mul(dy, b[1])), denominator),
            narrow);
      } else {
        const auto x = m.div(e.absolute(dx), b[0]),
                   y = m.div(e.absolute(dy), b[1]);
        if (kind == GradientKind::Radial)
          result = e.rounded_sqrt(m.add(m.sq(x), m.sq(y)), narrow);
        else
          result = m.rounded(kind == GradientKind::Diamond ? m.add(x, y)
                             : e.order(x, y) >= 0          ? x
                                                           : y,
                             narrow);
      }
      check(e.status());
      store(out, at, result);
    });
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    const auto& out = invocation.outputs[0];
    const bool narrow = type == ElementType::Float32;
    if (kind == GradientKind::Spread) {
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        const auto input = reader.number(0, 0, at);
        double value;
        if (method == "pad") {
          if (input >= 0 && input <= 1) {
            store(out, at, reader.bits(0, 0, at));
            return;
          }
          value = input < 0 ? 0 : 1;
        } else if (method == "repeat") {
          // Remainder modulo a power of two is exact for binary inputs.
          // The sole potentially inexact addition is the final RN stage.
          value = std::fmod(input, 1.0);
          if (value < 0)
            value += 1;
        } else {
          value = std::fmod(std::abs(input), 2.0);
          if (value > 1)
            value = 2 - value;
        }
        if (value == 0)
          value = 0;
        store(out, at, convert(value, narrow));
      });
      return;
    }
    if (kind != GradientKind::Angular) {
      if (kind == GradientKind::Lookup) {
        run_scalar<128>(kernel, invocation, work);
      } else {
        bool compact = true;
        for (unsigned port = 0; port < 2; ++port)
          for (unsigned component = 0; component < 2; ++component) {
            const auto value = std::abs(reader.number(port, 0, {component}));
            compact &= value == 0 || (value >= 0x1p-64 && value <= 0x1p64);
          }
        // Controls span at most 181 binary places including significands;
        // squared rational coordinate expressions and rounding fit 4096 bits.
        if (compact)
          run_scalar<64>(kernel, invocation, work);
        else
          run_scalar<384>(kernel, invocation, work);
      }
      return;
    }
    MathArena arena(invocation.scratch[0].allocation, work, refinements);
    auto& m = *arena;
    auto& e = m.exact;
    const std::array<Rational, 2> a{m.input(reader, 0, 0, {0}),
                                    m.input(reader, 0, 0, {1})};
    const auto phase = e.modulo(m.input(reader, 1, 0, {0}), 1);
    check(e.status());
    const std::array<Fast, 2> fa{Fast::point(reader.number(0, 0, {0})),
                                 Fast::point(reader.number(0, 0, {1}))};
    const auto fast_phase =
        Fast::point(std::fmod(reader.number(1, 0, {0}), 1.0));
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      e.reset();
      if (narrow || accelerated) {
        work.poll(160);
        const auto dx = fast_center(origin_x, at[1]) - fa[0];
        const auto dy = fast_center(origin_y, at[0]) - fa[1];
        Fast value{-INFINITY, INFINITY};
        if (auto angle = fast_turns(dy, dx)) {
          value = *angle - fast_phase;
          const auto whole = std::floor(value.low);
          value = whole == std::floor(value.high) ? value - Fast::point(whole)
                                                  : Fast{-INFINITY, INFINITY};
        }
        if (const auto accepted = fast_result(value, narrow, accelerated)) {
          store(out, at, *accepted);
          return;
        }
      }
      const auto dx = m.sub(m.center(origin_x, at[1]), a[0]);
      const auto dy = m.sub(m.center(origin_y, at[0]), a[1]);
      std::uint64_t result;
      if (Exact::zero(dx) && Exact::zero(dy)) {
        result = 0;
      } else {
        int octant = -1;
        if (Exact::zero(dy))
          octant = dx.negative ? 4 : 0;
        else if (Exact::zero(dx))
          octant = dy.negative ? 6 : 2;
        else if (e.order(e.absolute(dx), e.absolute(dy)) == 0)
          octant = dx.negative ? (dy.negative ? 5 : 3) : (dy.negative ? 7 : 1);
        if (octant >= 0) {
          result = m.rounded(e.modulo(m.sub(e.fraction(octant, 8), phase), 1),
                             narrow);
        } else {
          result = m.certify(narrow, [&](Interval value) {
            auto angle = m.d.interval(), tau = m.d.interval();
            auto offset_turns = m.d.interval(), turns = m.d.interval();
            m.atan2(angle, dy, dx);
            m.d.constant(tau, true);
            m.d.scale(tau, tau, 1);
            m.d.divide(turns, angle, tau);
            m.enclose(offset_turns, phase);
            m.d.subtract(turns, turns, offset_turns);
            auto& lower = m.d.number();
            auto& upper = m.d.number();
            m.d.shift(lower, turns.low, -static_cast<int>(m.d.precision), true);
            m.d.shift(upper, turns.high, -static_cast<int>(m.d.precision),
                      true);
            if (!m.d.active())
              return;
            if (m.d.compare(lower, upper) != 0) {
              m.d.retry();
              return;
            }
            auto whole = m.d.interval();
            m.d.shift(whole.low, lower, static_cast<int>(m.d.precision), true);
            m.d.copy(whole.high, whole.low);
            m.d.subtract(value, turns, whole);
          });
        }
      }
      check(e.status());
      store(out, at, result);
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_gradients(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    struct Entry {
      const char* name;
      GradientKind kind;
      unsigned inputs;
    };
    const Entry entries[] = {
        {"linear_gradient_coordinate", GradientKind::Linear, 2},
        {"radial_gradient_coordinate", GradientKind::Radial, 2},
        {"angular_gradient_coordinate", GradientKind::Angular, 2},
        {"diamond_gradient_coordinate", GradientKind::Diamond, 2},
        {"box_gradient_coordinate", GradientKind::Box, 2},
        {"spread_coordinate", GradientKind::Spread, 1},
        {"lookup_numeric_gradient", GradientKind::Lookup, 2}};
    for (const auto& entry : entries) {
      const bool canvas = entry.kind != GradientKind::Spread &&
                          entry.kind != GradientKind::Lookup;
      auto parameters =
          canvas ? canvas_parameters() : std::vector<OperationParameterSpec>{};
      if (entry.kind == GradientKind::Lookup)
        parameters.push_back({"dtype", OperationParameterType::String, false});
      if (entry.kind == GradientKind::Spread)
        parameters.push_back({"method", OperationParameterType::String, false});
      if (entry.kind == GradientKind::Angular) {
        parameters.push_back(
            {"center_policy", OperationParameterType::String, false});
        parameters.push_back(
            {"max_refinements", OperationParameterType::Int64, false});
      }
      const char* suffix[] = {"strict", "accelerated_apple_silicon",
                              "accelerated_x86_64"};
      for (unsigned profile = 0; profile < 3; ++profile) {
        check(registry->register_operation(operation(
            std::string("generation.") + entry.name + "_v1_" + suffix[profile],
            entry.inputs, parameters,
            {{canvas ? "t" : "values",
              tensor_schema(ElementType::Float64, {1})}},
            [kind = entry.kind, profile](const auto& inputs, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<Gradient>(kind, inputs, p, profile != 0);
            })));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
