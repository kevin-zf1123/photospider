#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/paths.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class PointKind { Grid, Jitter, Rejection, Bridson };
struct Points final : Program {
  PointKind kind;
  std::uint64_t nx = 1, ny = 1, capacity = 1, target = 1, candidates = 0;
  std::uint64_t seed = 0, stream = 0, attempts = 30, iterations = 2097153;
  std::uint64_t refinements = 4096;
  double jitter = 1;
  DenseLayout layout;
  Points(PointKind selected, const std::vector<OperationMetadata>& in,
         const Parameters& p)
      : kind(selected), layout(take(ps::generation::dynamic_points_schema(1))) {
    shape(in[0], {4});
    complete = true;
    if (kind == PointKind::Grid || kind == PointKind::Jitter) {
      nx = integer(p, "nx");
      ny = integer(p, "ny");
      capacity = target = size_product({nx, ny});
      if (capacity >
          static_cast<std::uint64_t>(integer(p, "max_points", 1048576)))
        throw Status{ErrorCode::ResourceExhausted, "grid point capacity limit",
                     FailureReason::CapacityLimit};
    } else {
      shape(in[1], {1});
      target = integer(p, "max_count", 1048576);
      capacity = std::max(UINT64_C(1), target);
      candidates = integer(p, "candidate_count");
      attempts = integer(p, "k", 30);
      iterations = integer(p, "max_iterations", 2097153);
    }
    seed = static_cast<std::uint64_t>(integer(p, "seed"));
    stream = static_cast<std::uint64_t>(integer(p, "stream"));
    jitter = real(p, "jitter", 1);
    refinements =
        static_cast<std::uint64_t>(integer(p, "max_refinements", 4096));
    const auto schema = take(ps::generation::dynamic_points_schema(capacity));
    layout = DenseLayout(schema);
    scratch_bytes = math_bytes() + layout.bytes;
    add_output(this, schema);
    if (kind == PointKind::Rejection || kind == PointKind::Bridson)
      add_output(this, tensor_schema(ElementType::Int64, {1}));
  }
  std::uint64_t math_bytes() const {
    return kind == PointKind::Grid || kind == PointKind::Jitter
               ? sizeof(ScalarMath<384>)
               : sizeof(Math);
  }
  template <unsigned Words>
  void grid(const Reader& reader, const Work& work, void* scratch,
            const Dense& points) const {
    using M = ScalarMath<Words>;
    using R = typename M::R;
    auto* memory = new (scratch) M(work);
    struct Destroy {
      M* p;
      ~Destroy() { p->~M(); }
    } destroy{memory};
    auto& m = *memory;
    auto& e = m.exact;
    const auto half = e.fraction(1, 2), amount = e.binary(bits(jitter));
    std::array<R, 4> bounds;
    for (unsigned i = 0; i < 4; ++i)
      bounds[i] = m.input(reader, 0, {i});
    const std::array<R, 2> step{
        m.div(m.sub(bounds[2], bounds[0]), e.integer(nx)),
        m.div(m.sub(bounds[3], bounds[1]), e.integer(ny))};
    const bool regular = kind == PointKind::Grid || jitter == 0;
    const auto mapped = [&](const R& u, const R& left, const R& right,
                            unsigned axis) {
      const auto value = m.rounded(m.add(left, m.mul(step[axis], u)), false);
      const bool upper = e.order(e.binary(value), right) >= 0;
      check(e.status());
      return upper ? bits(std::nextafter(number(m.rounded(right, false)),
                                         -INFINITY))
                   : value;
    };
    auto bottom = bounds[1];
    for (std::uint64_t j = 0; j < ny; ++j) {
      const auto top = m.add(bottom, step[1]);
      const auto y = regular ? mapped(half, bottom, top, 1) : 0;
      auto left = bounds[0];
      for (std::uint64_t i = 0; i < nx; ++i) {
        work.poll(10);
        const auto index = j * nx + i;
        std::array<std::uint64_t, 2> p;
        if (regular && j) {
          p = {points.get(0, 2 * i), y};
        } else {
          const auto right = m.add(left, step[0]);
          if (regular) {
            p = {mapped(half, left, right, 0), y};
          } else {
            const auto words = philox(i, j, 0, 3, seed, stream, 0);
            for (unsigned axis = 0; axis < 2; ++axis) {
              const auto u =
                  e.fraction(static_cast<std::int64_t>(words[axis] >> 11),
                             INT64_C(1) << 53);
              const auto t = m.add(half, m.mul(amount, m.sub(u, half)));
              p[axis] =
                  mapped(t, axis ? bottom : left, axis ? top : right, axis);
            }
          }
          left = right;
        }
        points.set(0, 2 * index, p[0]);
        points.set(0, 2 * index + 1, p[1]);
        points.set(1, index, index);
      }
      bottom = top;
    }
    check(e.status());
    points.set(2, 0, target);
  }
  static Rational uniform(Math& m, std::uint64_t word) {
    return m.exact.fraction(static_cast<std::int64_t>(word >> 11), INT64_C(1)
                                                                       << 53);
  }
  static std::uint64_t inside_map(Math& m, const Rational& u,
                                  const Rational& left, const Rational& right) {
    const auto value =
        m.rounded(m.add(left, m.mul(m.sub(right, left), u)), false);
    if (m.exact.order(m.value(number(value)), right) >= 0)
      return bits(std::nextafter(number(m.rounded(right, false)), -INFINITY));
    check(m.exact.status());
    return value;
  }
  static bool separated(Math& m, const Dense& points, std::uint64_t count,
                        const std::array<std::uint64_t, 2>& candidate,
                        const Rational& radius2, Fast threshold) {
    const std::array<Fast, 2> p{Fast::point(number(candidate[0])),
                                Fast::point(number(candidate[1]))};
    for (std::uint64_t i = 0; i < count; ++i) {
      m.work.poll(16);
      const auto x = number(points.get(0, 2 * i));
      const auto y = number(points.get(0, 2 * i + 1));
      const auto dx_fast = p[0] - Fast::point(x);
      const auto dy_fast = p[1] - Fast::point(y);
      const auto distance = dx_fast * dx_fast + dy_fast * dy_fast;
      if (distance.finite() && threshold.finite()) {
        if (distance.low >= threshold.high)
          continue;
        if (distance.high < threshold.low)
          return false;
      }
      const auto dx = m.sub(m.value(p[0].low), m.value(x));
      const auto dy = m.sub(m.value(p[1].low), m.value(y));
      const bool near = m.exact.order(m.add(m.sq(dx), m.sq(dy)), radius2) < 0;
      check(m.exact.status());
      if (near)
        return false;
    }
    return true;
  }
  static std::array<std::uint64_t, 2> annulus(
      Math& m, const std::array<Rational, 2>& parent, const Rational& radius2,
      std::uint64_t radial, std::uint64_t angular) {
    const auto u = uniform(m, radial), v = uniform(m, angular);
    auto r2 = m.mul(radius2, m.add(m.exact.integer(1), m.scale(u, 3)));
    const auto v53 = angular >> 11;
    std::array<std::uint64_t, 2> result;
    if ((v53 & ((UINT64_C(1) << 50) - 1)) == 0) {
      const unsigned octant = static_cast<unsigned>(v53 >> 50);
      constexpr int signs[2][8] = {{1, 1, 0, -1, -1, -1, 0, 1},
                                   {0, 1, 1, 1, 0, -1, -1, -1}};
      if (octant & 1)
        r2 = m.div(r2, m.exact.integer(2));
      for (unsigned axis = 0; axis < 2; ++axis)
        result[axis] =
            signs[axis][octant]
                ? m.affine_sqrt(parent[axis],
                                m.exact.integer(signs[axis][octant]), r2, false)
                : m.rounded(parent[axis], false);
      return result;
    }
    // Both coordinates enclose their complete real expression. Reusing the
    // radius and sin/cos enclosure introduces no intermediate rounding.
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < m.max_refinements;
         precision *= 2, ++attempt) {
      m.work.poll();
      m.d.reset();
      m.d.precision = precision;
      auto squared = m.d.interval(), rho = m.d.interval();
      auto angle = m.d.interval(), tau = m.d.interval(), phase = m.d.interval();
      auto sine = m.d.interval(), cosine = m.d.interval(),
           center = m.d.interval();
      auto coordinate = m.d.interval();
      m.enclose(squared, r2);
      m.sqrt(rho, squared);
      m.enclose(phase, v);
      m.d.constant(tau, true);
      m.d.scale(tau, tau, 1);
      m.d.multiply(angle, tau, phase);
      m.sincos(sine, cosine, angle);
      bool certified = true;
      for (unsigned axis = 0; axis < 2; ++axis) {
        m.d.multiply(coordinate, rho, axis ? sine : cosine);
        m.enclose(center, parent[axis]);
        m.d.add(coordinate, coordinate, center);
        result[axis] = m.d.rounded(coordinate.low, false);
        certified &= result[axis] == m.d.rounded(coordinate.high, false);
      }
      check(m.exact.status());
      if (m.d.fault == Directed::Fault::Failure)
        check(m.d.status());
      if (m.d.active() && certified)
        return result;
    }
    throw Status{ErrorCode::OperationFailed,
                 "Bridson refinement did not certify rounding",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* scratch =
        static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    Dense points{layout, scratch + math_bytes(), work};
    points.clear();
    if (kind == PointKind::Grid || kind == PointKind::Jitter) {
      bool compact = true;
      for (unsigned i = 0; i < 5; ++i) {
        const auto value = std::abs(i == 4 ? jitter : reader.number(0, 0, {i}));
        compact &= value == 0 || (value >= 0x1p-64 && value <= 0x1p64);
      }
      // Binary controls in this range span at most 181 places. Adding 40-bit
      // grid divisors and the 53-bit random fraction leaves every numerator,
      // denominator and pre-reduction product below 1024 bits.
      if (compact)
        grid<64>(reader, work, scratch, points);
      else
        grid<384>(reader, work, scratch, points);
      points.publish(kernel, invocation);
      return;
    }
    MathArena arena(scratch, work, refinements);
    auto& m = *arena;
    auto& e = m.exact;
    std::array<Rational, 4> bounds;
    std::array<double, 4> bound_values;
    for (unsigned i = 0; i < 4; ++i) {
      bounds[i] = m.input(reader, 0, 0, {i});
      bound_values[i] = reader.number(0, 0, {i});
    }
    std::uint64_t count = 0, reason = 0;
    const auto append_point = [&](const std::array<std::uint64_t, 2>& p) {
      points.set(0, 2 * count, p[0]);
      points.set(0, 2 * count + 1, p[1]);
      points.set(1, count, count);
      ++count;
    };
    const auto random_point = [&](std::uint64_t x, unsigned domain) {
      work.poll(10);
      const auto words = philox(x, 0, 0, domain, seed, stream, 0);
      return std::array<std::uint64_t, 2>{
          inside_map(m, uniform(m, words[0]), bounds[0], bounds[2]),
          inside_map(m, uniform(m, words[1]), bounds[1], bounds[3])};
    };
    if (target) {
      const auto radius = m.input(reader, 1, 0, {0}), radius2 = m.sq(radius);
      const auto fast_radius = Fast::point(reader.number(1, 0, {0}));
      const auto threshold = fast_radius * fast_radius;
      if (kind == PointKind::Rejection) {
        for (std::uint64_t i = 0; i < candidates && count < target; ++i) {
          const auto p = random_point(i, 4);
          if (separated(m, points, count, p, radius2, threshold))
            append_point(p);
        }
        reason = count == target ? 0 : 1;
      } else {
        append_point(random_point(0, 5));
        std::uint64_t head = 0, iteration = 0;
        while (count < target && head < count) {
          if (iteration == iterations)
            throw Status{ErrorCode::OperationFailed, "Bridson iteration limit",
                         DomainErrorCode{UINT64_C(0x47454e0001)}};
          const std::array<Rational, 2> parent{
              m.value(number(points.get(0, 2 * head))),
              m.value(number(points.get(0, 2 * head + 1)))};
          bool accepted = false;
          for (std::uint64_t a = 0; a < attempts; ++a) {
            work.poll(10);
            const auto words = philox(iteration + 1, a, 0, 5, seed, stream, 0);
            const auto p = annulus(m, parent, radius2, words[0], words[1]);
            bool inside = true;
            for (unsigned axis = 0; axis < 2; ++axis) {
              const double value = number(p[axis]);
              if (!std::isfinite(value))
                throw Status{ErrorCode::OperationFailed,
                             "Bridson candidate arithmetic overflow",
                             DomainErrorCode{UINT64_C(0x47454e0004)}};
              inside &=
                  value >= bound_values[axis] && value < bound_values[axis + 2];
            }
            check(e.status());
            if (inside && separated(m, points, count, p, radius2, threshold)) {
              append_point(p);
              accepted = true;
              break;
            }
          }
          if (!accepted)
            ++head;
          ++iteration;
        }
        reason = count == target ? 0 : 2;
      }
    }
    check(e.status());
    points.set(2, 0, count);
    points.publish(kernel, invocation);
    for (unsigned i = 0; i < kernel.writes.size(); ++i)
      if (kernel.writes[i].first == 1) {
        samples(invocation.outputs[i],
                [&](const Coordinate& at, std::uint64_t) {
                  work.poll();
                  store(invocation.outputs[i], at, reason);
                });
      }
  }
};
struct PointReport final : Program {
  explicit PointReport(const std::vector<OperationMetadata>& inputs) {
    require(
        inputs[0].result_schema && inputs[0].result_schema->tensors.size() == 3,
        "point report requires DynamicPoints");
    const auto& schema = *inputs[0].result_schema;
    require(schema.tensors[0].sample_shape().size() == 2,
            "DynamicPoints positions rank");
    const auto expected = take(ps::generation::dynamic_points_schema(
        schema.tensors[0].sample_shape()[0]));
    require(schema.same_schema(expected), "DynamicPoints schema directory");
    shape(inputs[1], {1}, false);
    require(tensor(inputs[1]).descriptor.element_type == ElementType::Int64,
            "point report reason must be Int64");
    complete = true;
    add_output(this, take(ps::generation::point_generation_report_schema()));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    const auto id =
        invocation.input_object_ids[kernel.groups[0].slots[0].second];
    const auto reason = reader.bits(1, 0, {0});
    for (unsigned i = 0; i < kernel.writes.size(); ++i)
      samples(invocation.outputs[i], [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        store(
            invocation.outputs[i], at,
            kernel.writes[i].second == 0 ? reason : (id >> (8 * at[0])) & 255);
      });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_point_generation(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    const char* names[] = {"grid_points", "jittered_grid_points",
                           "poisson_rejection_candidates",
                           "bridson_candidates"};
    for (unsigned i = 0; i < 4; ++i) {
      const auto kind = static_cast<PointKind>(i);
      const bool grid = i < 2;
      std::vector<OperationParameterSpec> parameters;
      if (grid) {
        parameters = {{"nx", OperationParameterType::Int64},
                      {"ny", OperationParameterType::Int64},
                      {"max_points", OperationParameterType::Int64, false}};
      } else {
        parameters = {{"max_count", OperationParameterType::Int64, false}};
        if (i == 2) {
          parameters.push_back(
              {"candidate_count", OperationParameterType::Int64});
        } else {
          parameters.push_back({"k", OperationParameterType::Int64, false});
          parameters.push_back(
              {"max_iterations", OperationParameterType::Int64, false});
          parameters.push_back(
              {"max_refinements", OperationParameterType::Int64, false});
        }
      }
      if (i) {
        parameters.push_back({"seed", OperationParameterType::Int64});
        parameters.push_back({"stream", OperationParameterType::Int64});
      }
      if (i == 1)
        parameters.push_back(
            {"jitter", OperationParameterType::Float64, false});
      std::vector<std::pair<std::string, SchemaTemplate>> outputs{
          {"points", take(ps::generation::dynamic_points_schema(1))}};
      if (!grid)
        outputs.emplace_back("stop_reason",
                             tensor_schema(ElementType::Int64, {1}));
      check(registry->register_operation(operation(
          std::string("generation.") + names[i] + "_v1_strict", grid ? 1 : 2,
          parameters, outputs, [kind](const auto& in, const auto& p) {
            return std::make_shared<Points>(kind, in, p);
          })));
    }
    auto report = operation(
        "generation.bind_point_generation_report_v1", 2, {},
        {{"report", take(ps::generation::point_generation_report_schema())}},
        [](const auto& in, const auto&) {
          return std::make_shared<PointReport>(in);
        },
        false, false);
    auto& points_input = report.traits.input_schema[0];
    points_input.element_type_mask = 0;
    points_input.result_schema_id = "photospider.ops.dynamic_points";
    points_input.result_schema_version = 1;
    check(registry->register_operation(std::move(report)));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
