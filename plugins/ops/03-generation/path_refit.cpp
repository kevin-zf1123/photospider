#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_support.hpp"
#include "03-generation/radical.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct Refit final : Program {
  bool fit, strict_topology, explicit_tangents;
  PathInfo input, output;
  double epsilon;
  std::uint64_t arena_bytes, max_segments, iterations, refinements;
  std::vector<DenseLayout> layouts;
  Refit(const std::vector<OperationMetadata>& in, const Parameters& params,
        bool fitting)
      : fit(fitting),
        strict_topology(text(params, "topology", "allow_change") ==
                        "reject_unproven"),
        explicit_tangents(text(params, "endpoint_tangent_policy", "chord") ==
                          "explicit"),
        input(path_info(*in[0].result_schema)),
        output(output_core(
            params,
            fitting ? checked_add(size_product({input.capacities[1], 3}),
                                  subpath_capacity(input))
                    : input.capacities[1],
            subpath_capacity(input))),
        epsilon(real(params, "epsilon_geom_px", 0.05)),
        arena_bytes(integer(params, "max_scratch_bytes", 64 * 1024 * 1024)),
        max_segments(integer(params, "max_segments", 1048576)),
        iterations(integer(params, "max_iterations", 64)),
        refinements(integer(params, "max_refinements", 4096)) {
    require(!input.primitive, "polyline editing requires CoreVerbs");
    require(in.size() <= (fitting ? 3U : 2U),
            "too many polyline editing inputs");
    require(epsilon >= 0 && (!fit || epsilon > 0) && max_segments &&
                iterations && refinements,
            "invalid polyline fitting budgets");
    require(text(params, "topology", "allow_change") == "allow_change" ||
                strict_topology,
            "unknown topology policy");
    require(text(params, "endpoint_tangent_policy", "chord") == "chord" ||
                explicit_tangents,
            "unknown endpoint tangent policy");
    if (in.size() >= 2) {
      shape(in[1], {input.capacities[1]}, false);
      require(tensor(in[1]).descriptor.element_type == ElementType::UInt8,
              "locked field requires UInt8");
    }
    if (explicit_tangents) {
      require(fit && in.size() == 3,
              "explicit tangents require locked and tangent fields");
      shape(in[2], {input.capacities[1], 2, 2});
    }
    add_output(this, path_schema(output));
    add_output(this, fit ? take(ps::generation::path_fit_report_schema())
                         : take(ps::generation::geometry_report_schema(1)));
    scratch_bytes = checked_add(sizeof(Math), arena_bytes);
    for (const auto& value : outputs) {
      layouts.emplace_back(*value.result_schema);
      scratch_bytes += layouts.back().bytes;
    }
    complete = true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arithmetic(base, work, refinements);
    auto& m = *arithmetic;
    const Dense path{layouts[0], base + sizeof(Math), work};
    const Dense report{layouts[1], base + sizeof(Math) + layouts[0].bytes,
                       work};
    path.clear();
    report.clear();
    std::pmr::monotonic_buffer_resource memory(
        base + sizeof(Math) + layouts[0].bytes + layouts[1].bytes, arena_bytes,
        std::pmr::null_memory_resource());
    std::pmr::unsynchronized_pool_resource pool(&memory);
    Radical r(m, &pool);
    RadicalPlane p(r);
    CoreWriter writer(path, output);
    PathCursor source(reader, input, m);
    std::pmr::vector<RadicalPoint> points(&pool);
    std::pmr::vector<RadicalPoint> start_tangents(&pool), end_tangents(&pool);
    std::pmr::vector<std::size_t> nonzero_edges(&pool);
    std::pmr::vector<Radical::Id> cumulative(&pool);
    std::pmr::vector<std::uint8_t> kept(&pool);
    struct Range {
      std::uint64_t a, b;
    };
    std::pmr::vector<Range> pending(&pool);
    auto error_squared = r.integer(0);
    std::uint64_t segments = 0;
    const auto epsilon_squared = r.sq(r.value(epsilon));
    for (std::uint64_t subpath = 0; subpath < source.subpaths(); ++subpath) {
      const auto begin = static_cast<std::uint64_t>(
          reader.index(0, 1, {source.begin(subpath)}));
      const auto end =
          static_cast<std::uint64_t>(reader.index(0, 1, {source.end(subpath)}));
      const auto count = end - begin;
      const bool closed = source.closed(subpath);
      points.clear();
      cumulative.clear();
      kept.clear();
      pending.clear();
      start_tangents.clear();
      end_tangents.clear();
      nonzero_edges.clear();
      for (std::uint64_t j = begin; j < end; ++j) {
        points.push_back(
            {r.input(reader, 0, 2, {j, 0}), r.input(reader, 0, 2, {j, 1})});
      }
      if (closed && !points.empty()) {
        points.push_back(points.front());
      }
      writer.begin(closed);
      writer.point(1, reader.bits(0, 2, {begin, 0}),
                   reader.bits(0, 2, {begin, 1}));
      if (!fit) {
        kept.resize(points.size(), 0);
        cumulative.push_back(0);
        for (std::size_t j = 1; j < points.size(); ++j) {
          cumulative.push_back(
              r.add(cumulative.back(),
                    r.sqrt(p.norm(p.sub(points[j], points[j - 1])))));
        }
        kept.front() = kept.back() = 1;
        if (closed && count > 1) {
          std::uint64_t anchor = 1;
          auto farthest = p.norm(p.sub(points[1], points[0]));
          for (std::uint64_t j = 2; j < count; ++j) {
            const auto value = p.norm(p.sub(points[j], points[0]));
            if (r.compare(value, farthest) > 0) {
              farthest = value;
              anchor = j;
            }
          }
          kept[anchor] = 1;
        }
        if (inputs.size() >= 2) {
          for (std::uint64_t j = 0; j < count; ++j) {
            if (reader.bits(1, 0, {begin + j})) {
              kept[j] = 1;
            }
          }
        }
        std::uint64_t previous = 0;
        for (std::uint64_t j = 1; j < kept.size(); ++j) {
          if (kept[j]) {
            pending.push_back({previous, j});
            previous = j;
          }
        }
        while (!pending.empty()) {
          const auto range = pending.back();
          pending.pop_back();
          if (range.b - range.a <= 1) {
            continue;
          }
          const auto length = r.sub(cumulative[range.b], cumulative[range.a]);
          const auto chord = p.sub(points[range.b], points[range.a]);
          Radical::Id maximum_error = 0;
          std::uint64_t split = range.a + 1;
          for (auto j = range.a + 1; j < range.b; ++j) {
            work.poll();
            const auto s = r.sub(cumulative[j], cumulative[range.a]);
            const auto numerator =
                p.sub(p.mul(p.sub(points[j], points[range.a]), length),
                      p.mul(chord, s));
            const auto error = p.norm(numerator);
            if (r.compare(error, maximum_error) > 0) {
              maximum_error = error;
              split = j;
            }
          }
          if (r.compare(maximum_error, r.mul(epsilon_squared, r.sq(length))) <=
              0) {
            if (r.sign(length)) {
              error_squared =
                  r.max(error_squared, r.div(maximum_error, r.sq(length)));
            }
          } else {
            kept[split] = 1;
            pending.push_back({split, range.b});
            pending.push_back({range.a, split});
          }
        }
        for (std::uint64_t j = 1; j < count; ++j) {
          if (kept[j]) {
            operational_capacity(++segments <= max_segments);
            writer.point(2, reader.bits(0, 2, {begin + j, 0}),
                         reader.bits(0, 2, {begin + j, 1}));
          }
        }
      } else {
        // Versioned envelope fitter: interpolate every supplied vertex. The
        // continuous certificate is a convex capsule bound, not sampled error.
        for (std::uint64_t j = 1; j < points.size(); ++j) {
          operational_capacity(++segments <= max_segments);
          const auto a = points[j - 1], b = points[j], direction = p.sub(b, a);
          const auto squared = p.norm(direction);
          RadicalPoint first, second, first_tangent{0, 0}, last_tangent{0, 0};
          if (!explicit_tangents || !r.sign(squared)) {
            first =
                p.add(a, p.mul(direction, r.constant(m.exact.fraction(1, 3))));
            second =
                p.add(a, p.mul(direction, r.constant(m.exact.fraction(2, 3))));
          } else {
            const auto first_index = begin + (j - 1) % count,
                       last_index = begin + j % count;
            const RadicalPoint ta{r.input(reader, 2, 0, {first_index, 1, 0}),
                                  r.input(reader, 2, 0, {first_index, 1, 1})};
            const RadicalPoint tb{r.input(reader, 2, 0, {last_index, 0, 0}),
                                  r.input(reader, 2, 0, {last_index, 0, 1})};
            first_tangent = ta;
            last_tangent = tb;
            auto handle = r.min(r.div(r.sqrt(squared), r.integer(3)),
                                r.div(r.value(epsilon), r.integer(4)));
            first = p.add(a, p.mul(ta, r.div(handle, r.sqrt(p.norm(ta)))));
            second = p.sub(b, p.mul(tb, r.div(handle, r.sqrt(p.norm(tb)))));
          }
          std::uint64_t controls[6]{};
          bool certified = false;
          for (std::uint64_t attempt = 0; attempt < iterations; ++attempt) {
            work.poll();
            const RadicalPoint candidates[] = {first, second, b};
            Radical::Id bound = 0;
            bool tangent_constraints = true;
            for (unsigned k = 0; k < 3; ++k) {
              controls[2 * k] = r.round(candidates[k].x);
              controls[2 * k + 1] = r.round(candidates[k].y);
              const RadicalPoint published{
                  r.constant(m.exact.binary(controls[2 * k])),
                  r.constant(m.exact.binary(controls[2 * k + 1]))};
              if (explicit_tangents && r.sign(squared) && k < 2) {
                const auto derivative =
                    k ? p.sub(b, published) : p.sub(published, a);
                const auto tangent = k ? last_tangent : first_tangent;
                tangent_constraints &= !r.sign(p.cross(derivative, tangent)) &&
                                       r.sign(p.dot(derivative, tangent)) > 0;
              }
              const auto relative = p.sub(published, a);
              const auto t =
                  r.sign(squared)
                      ? r.max(0, r.min(1, r.div(p.dot(relative, direction),
                                                squared)))
                      : 0;
              bound =
                  r.max(bound, p.norm(p.sub(relative, p.mul(direction, t))));
            }
            if (tangent_constraints && r.compare(bound, epsilon_squared) <= 0) {
              error_squared = r.max(error_squared, bound);
              certified = true;
              break;
            }
            if (!explicit_tangents) {
              break;
            }
            const auto half = r.constant(m.exact.fraction(1, 2));
            first = p.add(a, p.mul(p.sub(first, a), half));
            second = p.add(b, p.mul(p.sub(second, b), half));
          }
          if (!certified) {
            invalid_quality(
                "cubic envelope fitter cannot certify its bound and tangent "
                "constraints");
          }
          writer.append(4, controls, 3);
          if (inputs.size() >= 2) {
            const RadicalPoint published_first{
                r.constant(m.exact.binary(controls[0])),
                r.constant(m.exact.binary(controls[1]))};
            const RadicalPoint published_second{
                r.constant(m.exact.binary(controls[2])),
                r.constant(m.exact.binary(controls[3]))};
            auto outgoing = p.sub(published_first, a);
            if (!r.sign(p.norm(outgoing))) {
              outgoing = p.sub(published_second, a);
            }
            if (!r.sign(p.norm(outgoing))) {
              outgoing = direction;
            }
            auto incoming = p.sub(b, published_second);
            if (!r.sign(p.norm(incoming))) {
              incoming = p.sub(b, published_first);
            }
            if (!r.sign(p.norm(incoming))) {
              incoming = direction;
            }
            start_tangents.push_back(outgoing);
            end_tangents.push_back(incoming);
            if (r.sign(squared)) {
              nonzero_edges.push_back(j - 1);
            }
          }
        }
        if (inputs.size() >= 2) {
          const auto edge_count = start_tangents.size();
          auto positive_collinear = [&](RadicalPoint a, RadicalPoint b) {
            return !r.sign(p.cross(a, b)) && r.sign(p.dot(a, b)) > 0;
          };
          std::size_t next_edge = 0;
          auto incoming = closed && !nonzero_edges.empty()
                              ? nonzero_edges.back()
                              : edge_count;
          for (std::size_t vertex = 0; vertex < count; ++vertex) {
            work.poll();
            // Traverse nonzero edges once, including repeated-point runs at
            // the closed seam, without changing the published source rows.
            while (next_edge < nonzero_edges.size() &&
                   nonzero_edges[next_edge] < vertex) {
              work.poll();
              incoming = nonzero_edges[next_edge++];
            }
            const auto outgoing =
                next_edge < nonzero_edges.size()   ? nonzero_edges[next_edge]
                : closed && !nonzero_edges.empty() ? nonzero_edges.front()
                                                   : edge_count;
            if (!reader.bits(1, 0, {begin + vertex}) ||
                incoming == edge_count || outgoing == edge_count) {
              continue;
            }
            const auto source_in =
                p.sub(points[incoming + 1], points[incoming]);
            const auto source_out =
                p.sub(points[outgoing + 1], points[outgoing]);
            if (!positive_collinear(source_in, source_out) &&
                positive_collinear(end_tangents[incoming],
                                   start_tangents[outgoing])) {
              invalid_quality(
                  "cubic envelope fitter cannot preserve a locked corner");
            }
          }
        }
      }
      if (closed) {
        writer.append(5, nullptr, 0);
      }
    }
    writer.finish();
    const bool identity = !r.sign(error_squared);
    if (strict_topology && !identity) {
      invalid_quality(
          "editing topology requires an exact-identity certificate");
    }
    const auto error = r.sqrt(error_squared);
    auto bound = number(r.round(error));
    if (r.compare(r.value(bound), error) < 0) {
      bound = std::nextafter(bound, std::numeric_limits<double>::infinity());
    }
    if (fit) {
      report.set(0, 0, bits(bound));
      report.set(1, 0, identity ? 1 : 0);
      write_id(report, 2, 0, source_id(reader, 0));
    } else {
      geometry_report(report, reader, 1, bound, 0, 0, identity ? 1 : 0);
    }
    path.publish(kernel, invocation, 0);
    report.publish(kernel, invocation, 1);
  }
};
struct TransformReport final : Program {
  PathInfo info;
  DenseLayout layout{SchemaTemplate{}};
  explicit TransformReport(const std::vector<OperationMetadata>& in)
      : info(path_info(*in[0].result_schema)) {
    require(!info.primitive,
            "stroke outline transform report requires a CoreVerbs polygon");
    shape(in[1], {2, 3});
    require(in[2].result_schema &&
                in[2].result_schema->id == "photospider.ops.geometry_report" &&
                in[2].result_schema->version == 1 &&
                in[2].result_schema->tensors.size() == 4,
            "outline transform requires GeometryReport v1");
    const auto source_shape = in[2].result_schema->tensors[3].sample_shape();
    require(source_shape.size() == 2 && source_shape[1] == 8,
            "report source directory");
    const auto expected =
        take(ps::generation::geometry_report_schema(source_shape[0]));
    require(expected.same_schema(*in[2].result_schema),
            "report directory mismatch");
    add_output(this, take(ps::generation::geometry_report_schema(3)));
    layout = DenseLayout(*outputs[0].result_schema);
    scratch_bytes = sizeof(Math) + layout.bytes + 8 * 1024 * 1024;
    complete = true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arithmetic(base, work);
    auto& m = *arithmetic;
    const Dense report{layout, base + sizeof(Math), work};
    report.clear();
    std::pmr::monotonic_buffer_resource memory(
        base + sizeof(Math) + layout.bytes, 8 * 1024 * 1024,
        std::pmr::null_memory_resource());
    std::pmr::unsynchronized_pool_resource pool(&memory);
    Radical r(m, &pool);
    const auto a = r.input(reader, 1, 0, {0, 0}),
               b = r.input(reader, 1, 0, {0, 1});
    const auto c = r.input(reader, 1, 0, {1, 0}),
               d = r.input(reader, 1, 0, {1, 1});
    const auto trace = r.add(r.add(r.sq(a), r.sq(b)), r.add(r.sq(c), r.sq(d)));
    const auto determinant = r.sub(r.mul(a, d), r.mul(b, c));
    const auto norm = r.sqrt(r.div(
        r.add(trace, r.sqrt(r.sub(r.sq(trace),
                                  r.mul(r.integer(4), r.sq(determinant))))),
        r.integer(2)));
    auto displacement = m.exact.integer(0);
    const auto count = path_counts(reader, 0, false);
    for (std::uint64_t j = 0; j < count[1]; ++j) {
      work.poll();
      const auto x = m.input(reader, 0, 2, {j, 0}),
                 y = m.input(reader, 0, 2, {j, 1});
      auto squared = m.exact.integer(0);
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto exact =
            m.add(m.add(m.mul(m.input(reader, 1, 0, {axis, 0}), x),
                        m.mul(m.input(reader, 1, 0, {axis, 1}), y)),
                  m.input(reader, 1, 0, {axis, 2}));
        const auto delta =
            m.sub(exact, m.exact.binary(m.rounded(exact, false)));
        squared = m.add(squared, m.mul(delta, delta));
      }
      if (m.exact.order(squared, displacement) > 0) {
        displacement = squared;
      }
    }
    const auto publication = r.sqrt(r.constant(displacement));
    const auto upward = [&](Radical::Id value) {
      auto number_value = number(r.round(value));
      if (r.compare(r.value(number_value), value) < 0) {
        number_value = std::nextafter(number_value,
                                      std::numeric_limits<double>::infinity());
      }
      return bits(number_value);
    };
    for (unsigned stage = 0; stage < 3; ++stage) {
      auto error = r.mul(norm, r.input(reader, 2, 0, {stage}));
      if (stage == 2) {
        error = r.add(error, publication);
      }
      report.set(0, stage, upward(error));
    }
    report.set(1, 0, !r.sign(publication) ? reader.bits(2, 1, {0}) : 0);
    report.set(2, 0, reader.bits(2, 2, {0}));
    for (unsigned j = 0; j < 3; ++j) {
      write_id(report, 3, j, source_id(reader, j));
    }
    report.publish(kernel, invocation, 0);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation
namespace ps::plugin_internal {
Status register_path_refit(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    for (bool fit : {false, true}) {
      auto parameters = path_capacity_parameters();
      append(
          &parameters,
          {{"epsilon_geom_px", OperationParameterType::Float64, false},
           {"topology", OperationParameterType::String, false},
           {"max_segments", OperationParameterType::Int64, false},
           {"endpoint_tangent_policy", OperationParameterType::String, false},
           {"max_iterations", OperationParameterType::Int64, false},
           {"max_scratch_bytes", OperationParameterType::Int64, false},
           {"max_refinements", OperationParameterType::Int64, false}});
      for (unsigned arity = 1; arity <= (fit ? 3U : 2U); ++arity) {
        const auto key = std::string(fit ? "path.fit_cubic_polyline_envelope"
                                         : "path.simplify_polyline") +
                         (arity == 1   ? ""
                          : arity == 2 ? "_locked"
                                       : "_tangents") +
                         "_v1_strict";
        auto op = operation(
            key, arity, parameters,
            {{"path", take(ps::generation::core_path_schema({}))},
             {"report", fit ? take(ps::generation::path_fit_report_schema())
                            : take(ps::generation::geometry_report_schema(1))}},
            [fit](const auto& in, const auto& params) {
              return std::make_shared<Refit>(in, params, fit);
            },
            false, false);
        any_path_input(&op, 0);
        check(registry->register_operation(std::move(op)));
      }
    }
    auto report = operation(
        "path.transform_outline_report_v1_strict", 3, {},
        {{"report", take(ps::generation::geometry_report_schema(3))}},
        [](const auto& in, const auto&) {
          return std::make_shared<TransformReport>(in);
        },
        false, false);
    any_path_input(&report, 0);
    report.traits.input_schema[2].element_type_mask = 0;
    report.traits.input_schema[2].result_schema_id =
        "photospider.ops.geometry_report";
    report.traits.input_schema[2].result_schema_version = 1;
    return registry->register_operation(std::move(report));
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
