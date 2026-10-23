#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/polygon_arrangement.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct BezierLeaf final {
  std::array<ExactPoint, 4> p;
  Rational begin, end;
  unsigned degree = 1, depth = 0;
};
bool sum_sqrts_at_most(Math& m, const Rational& a, const Rational& b,
                       const Rational& limit) {
  const auto rest = m.sub(m.sq(limit), m.add(a, b));
  return m.exact.order(rest, m.exact.integer(0)) >= 0 &&
         m.exact.order(m.sq(rest), m.scale(m.mul(a, b), 4)) >= 0;
}
double upper_sum(Math& m, double a, double b) {
  const auto exact = m.add(m.value(a), m.value(b));
  auto result = number(m.rounded(exact, false));
  if (m.exact.order(m.value(result), exact) < 0) {
    result = std::nextafter(result, std::numeric_limits<double>::infinity());
  }
  return result;
}
struct Flatten final : Program {
  PathInfo input, output;
  std::uint64_t max_segments, max_depth, limb_capacity;
  double epsilon;
  bool strict_topology;
  std::array<DenseLayout, 4> layouts;
  Flatten(const std::vector<OperationMetadata>& in, const Parameters& p)
      : input(path_info(*in[0].result_schema)),
        output(output_core(p, 4096 + subpath_capacity(input),
                           subpath_capacity(input))),
        max_segments(integer(p, "max_segments", 1048576)),
        max_depth(integer(p, "max_depth", 24)),
        limb_capacity(integer(p, "limb_capacity", max_depth / 32 + 1)),
        epsilon(real(p, "epsilon_geom_px", .05)),
        strict_topology(text(p, "topology", "allow_change") ==
                        "reject_unproven"),
        layouts{DenseLayout(path_schema(output)),
                DenseLayout(take(ps::generation::path_samples_schema(
                    output.capacities[1], output.capacities[2]))),
                DenseLayout(take(ps::generation::geometry_report_schema(1))),
                DenseLayout(take(ps::generation::path_source_map_schema(
                    output.capacities[0], 1, limb_capacity)))} {
    require(!input.primitive, "Bezier flattening requires CoreVerbs");
    const auto topology = text(p, "topology", "allow_change");
    require(topology == "allow_change" || topology == "reject_unproven",
            "unknown topology policy");
    complete = true;
    add_output(this, path_schema(output));
    add_output(this, take(ps::generation::path_samples_schema(
                         output.capacities[1], output.capacities[2])));
    add_output(this, take(ps::generation::geometry_report_schema(1)));
    add_output(this, take(ps::generation::path_source_map_schema(
                         output.capacities[0], 1, limb_capacity)));
    scratch_bytes = sizeof(EvaluationScratch);
    for (const auto& layout : layouts) {
      scratch_bytes = checked_add(scratch_bytes, layout.bytes);
    }
    scratch_bytes = checked_add(
        scratch_bytes, size_product({max_depth + 2, sizeof(BezierLeaf)}));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* bytes = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    EvaluationArena arithmetic(bytes, work);
    auto& m = arithmetic.p->math;
    const auto epsilon_q = m.value(epsilon);
    std::uint64_t offset = sizeof(EvaluationScratch);
    Dense path{layouts[0], bytes + offset, work};
    offset += layouts[0].bytes;
    Dense samples_out{layouts[1], bytes + offset, work};
    offset += layouts[1].bytes;
    Dense report{layouts[2], bytes + offset, work};
    offset += layouts[2].bytes;
    Dense mapping{layouts[3], bytes + offset, work};
    offset += layouts[3].bytes;
    path.clear();
    samples_out.clear();
    report.clear();
    mapping.clear();
    CoreWriter writer(path, output);
    const PathCursor source(reader, input, m);
    auto* stack = reinterpret_cast<BezierLeaf*>(bytes + offset);
    std::uint64_t point_count = 0, segments = 0;
    double bound = 0;
    bool identity = true;
    const auto sample = [&](std::uint64_t x, std::uint64_t y, std::int64_t row,
                            const Rational& t) {
      operational_capacity(point_count < output.capacities[1]);
      samples_out.set(0, 2 * point_count, x);
      samples_out.set(0, 2 * point_count + 1, y);
      samples_out.set(1, point_count, static_cast<std::uint64_t>(row));
      samples_out.set(2, point_count++, m.rounded(t, false));
    };
    for (std::uint64_t s = 0; s < source.subpaths(); ++s) {
      writer.begin(source.closed(s));
      samples_out.set(3, s, point_count);
      const auto x = source.isolated(s, 0), y = source.isolated(s, 1);
      writer.point(1, x, y);
      sample(x, y, source.first(s) == source.end(s) ? -1 : source.first(s),
             m.exact.integer(0));
      for (auto row = source.first(s); row < source.end(s); ++row) {
        const auto curve = source.curve(row);
        BezierLeaf initial;
        initial.degree = curve.degree;
        initial.begin = m.exact.integer(0);
        initial.end = m.exact.integer(1);
        for (unsigned j = 0; j <= curve.degree; ++j) {
          initial.p[j] = {m.input(reader, 0, 2, {curve.control[j], 0}),
                          m.input(reader, 0, 2, {curve.control[j], 1})};
        }
        std::uint64_t active = 1;
        new (stack) BezierLeaf(std::move(initial));
        while (active) {
          work.poll();
          auto leaf = stack[--active];
          stack[active].~BezierLeaf();
          auto deviation = m.exact.integer(0), publication = m.exact.integer(0);
          std::uint64_t endpoint[2]{};
          for (unsigned j = 0; j <= leaf.degree; ++j) {
            const auto t = m.exact.fraction(j, leaf.degree);
            const auto dx = m.sub(
                leaf.p[j].x, m.lerp(leaf.p[0].x, leaf.p[leaf.degree].x, t));
            const auto dy = m.sub(
                leaf.p[j].y, m.lerp(leaf.p[0].y, leaf.p[leaf.degree].y, t));
            deviation = m.max(deviation, m.add(m.sq(dx), m.sq(dy)));
          }
          for (unsigned endpoint_index = 0; endpoint_index < 2;
               ++endpoint_index) {
            const auto& p = leaf.p[endpoint_index ? leaf.degree : 0];
            const auto px = m.rounded(p.x, false), py = m.rounded(p.y, false);
            const auto dx = m.sub(m.exact.binary(px), p.x),
                       dy = m.sub(m.exact.binary(py), p.y);
            publication = m.max(publication, m.add(m.sq(dx), m.sq(dy)));
            if (endpoint_index) {
              endpoint[0] = px;
              endpoint[1] = py;
            }
          }
          if (sum_sqrts_at_most(m, deviation, publication, epsilon_q)) {
            operational_capacity(segments < max_segments &&
                                 segments < output.capacities[0]);
            const bool unchanged =
                Exact::zero(deviation) && Exact::zero(publication);
            identity &= unchanged;
            if (strict_topology && !unchanged) {
              invalid_quality(
                  "flatten topology proof requires exact geometric identity");
            }
            bound = std::max(bound, upper_sum(m, upward_sqrt(m, deviation),
                                              upward_sqrt(m, publication)));
            const auto target = writer.verbs;
            const bool closing = reader.bits(0, 0, {row}) == 5;
            if (closing) {
              writer.append(5, nullptr, 0);
            } else {
              writer.point(2, endpoint[0], endpoint[1]);
              sample(endpoint[0], endpoint[1], row, leaf.end);
            }
            mapping.set(0, 4 * segments, target);
            mapping.set(0, 4 * segments + 2, s);
            mapping.set(0, 4 * segments + 3, row);
            write_rational(mapping, 2, 3, 4, segments, 0, limb_capacity,
                           leaf.begin);
            write_rational(mapping, 2, 3, 4, segments, 1, limb_capacity,
                           m.sub(leaf.end, leaf.begin));
            ++segments;
          } else {
            if (leaf.depth >= max_depth) {
              invalid_quality(
                  "Bezier flattening depth cannot meet geometry bound");
            }
            const auto middle =
                m.div(m.add(leaf.begin, leaf.end), m.exact.integer(2));
            BezierLeaf left, right;
            left.degree = right.degree = leaf.degree;
            left.depth = right.depth = leaf.depth + 1;
            left.begin = leaf.begin;
            left.end = middle;
            right.begin = middle;
            right.end = leaf.end;
            auto controls = leaf.p;
            left.p[0] = controls[0];
            right.p[leaf.degree] = controls[leaf.degree];
            for (unsigned level = leaf.degree; level > 0; --level) {
              for (unsigned j = 0; j < level; ++j) {
                controls[j].x = m.div(m.add(controls[j].x, controls[j + 1].x),
                                      m.exact.integer(2));
                controls[j].y = m.div(m.add(controls[j].y, controls[j + 1].y),
                                      m.exact.integer(2));
              }
              left.p[leaf.degree - level + 1] = controls[0];
              right.p[level - 1] = controls[level - 1];
            }
            new (stack + active++) BezierLeaf(std::move(right));
            new (stack + active++) BezierLeaf(std::move(left));
          }
          check(m.exact.status());
        }
      }
    }
    writer.finish();
    for (auto s = source.subpaths(); s <= output.capacities[2]; ++s) {
      samples_out.set(3, s, point_count);
    }
    samples_out.set(4, 0, point_count);
    samples_out.set(4, 1, source.subpaths());
    write_id(samples_out, 5, 0, source_id(reader, 0));
    write_id(mapping, 1, 0, source_id(reader, 0));
    mapping.set(5, 0, segments);
    mapping.set(5, 1, 1);
    geometry_report(report, reader, 1, bound, 0, 0, identity ? 1 : 0);
    path.publish(kernel, invocation, 0);
    samples_out.publish(kernel, invocation, 1);
    report.publish(kernel, invocation, 2);
    mapping.publish(kernel, invocation, 3);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_flatten(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    auto parameters = path_capacity_parameters();
    append(&parameters,
           {{"max_segments", OperationParameterType::Int64, false},
            {"max_depth", OperationParameterType::Int64, false},
            {"limb_capacity", OperationParameterType::Int64, false},
            {"epsilon_geom_px", OperationParameterType::Float64, false},
            {"topology", OperationParameterType::String, false}});
    auto definition = operation(
        "path.flatten_bezier_path_v1_strict", 1, parameters,
        {{"path", take(ps::generation::core_path_schema({}))},
         {"samples", take(ps::generation::path_samples_schema(1, 1))},
         {"report", take(ps::generation::geometry_report_schema(1))},
         {"source_map", take(ps::generation::path_source_map_schema(1, 1, 1))}},
        [](const auto& in, const auto& p) {
          return std::make_shared<Flatten>(in, p);
        },
        false, false);
    any_path_input(&definition, 0);
    check(registry->register_operation(std::move(definition)));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
