#include <algorithm>
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
enum class PolygonKind { Exact, Publish, Float, Grid, GridFloat, FillDistance };
void polygon_schema(const OperationMetadata& input, bool exact_allowed) {
  require(input.result_schema != nullptr,
          "polygon input requires a Result schema");
  const auto& schema = *input.result_schema;
  if (schema.id == "photospider.ops.path_core") {
    require(!path_info(schema).primitive, "polygon requires CoreVerbs");
  } else {
    require(exact_allowed && schema.id == "photospider.ops.exact_polygon" &&
                schema.version == 1 && schema.tensors.size() == 5,
            "polygon requires CoreVerbs or exact_polygon v1");
    for (const auto& member : schema.tensors) {
      require(!member.sample_shape().empty(), "exact polygon member rank");
    }
    const auto& dimensions = schema.tensors[2].sample_shape();
    require(dimensions.size() == 5, "exact polygon limb rank");
    const auto expected = take(ps::generation::exact_polygon_schema(
        schema.tensors[0].sample_shape()[0],
        schema.tensors[3].sample_shape()[0] - 1, dimensions[3]));
    require(schema.same_schema(expected), "exact polygon directory mismatch");
  }
}
struct PolygonProgram final : Program {
  PolygonKind kind;
  BooleanKind operation_kind;
  bool evenodd_a, evenodd_b;
  std::uint64_t max_events, max_vertices, max_contours, limbs, arena_bytes;
  double spacing, ox, oy, max_distance;
  Canvas canvas;
  PathInfo path_output;
  std::vector<DenseLayout> layouts;
  PolygonProgram(PolygonKind selected, const std::vector<OperationMetadata>& in,
                 const Parameters& p)
      : kind(selected),
        operation_kind(boolean_kind(p)),
        evenodd_a(fill_evenodd(p, selected == PolygonKind::FillDistance
                                      ? "fill_rule"
                                      : "fill_rule_a")),
        evenodd_b(fill_evenodd(p, "fill_rule_b")),
        max_events(integer(p, "max_events", 65536)),
        max_vertices(integer(p, "max_vertices", 4096)),
        max_contours(integer(p, "max_contours", 256)),
        limbs(integer(p, "limb_capacity", 128)),
        arena_bytes(integer(p, "max_scratch_bytes", 64 * 1024 * 1024)),
        spacing(real(p, "grid_spacing", 1)),
        ox(real(p, "grid_origin_x")),
        oy(real(p, "grid_origin_y")),
        max_distance(real(p, "max_distance")),
        canvas(selected == PolygonKind::FillDistance
                   ? p
                   : Parameters{{"height", std::int64_t{1}},
                                {"width", std::int64_t{1}}}) {
    for (const auto& value : in) {
      polygon_schema(
          value, kind == PolygonKind::Exact || kind == PolygonKind::Publish);
    }
    if (kind == PolygonKind::Publish) {
      require(in[0].result_schema->id == "photospider.ops.exact_polygon",
              "exact publication requires exact_polygon");
    }
    const auto policy = text(p, "attribute_policy", "reject_unmappable");
    require(policy == "reject_unmappable" || policy == "drop_unmappable",
            "unknown attribute policy");
    complete = kind != PolygonKind::FillDistance;
    if (kind == PolygonKind::FillDistance) {
      add_output(this, tensor_schema(canvas.type, {canvas.height, canvas.width},
                                     true));
    } else {
      if (kind == PolygonKind::Exact) {
        add_output(this, take(ps::generation::exact_polygon_schema(
                             max_vertices, max_contours, limbs)));
      } else if (kind == PolygonKind::Grid) {
        add_output(this, take(ps::generation::grid_polygon_schema(
                             max_vertices, max_contours)));
      } else {
        path_output = output_core(p, max_vertices, max_contours);
        add_output(this, path_schema(path_output));
      }
      add_output(this, take(ps::generation::geometry_report_schema(in.size())));
      for (const auto& value : outputs) {
        layouts.emplace_back(*value.result_schema);
      }
    }
    scratch_bytes = sizeof(Math);
    for (const auto& layout : layouts) {
      scratch_bytes = checked_add(scratch_bytes, layout.bytes);
    }
    scratch_bytes = checked_add(scratch_bytes, arena_bytes);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* bytes = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arithmetic(bytes, work);
    auto& m = *arithmetic;
    std::uint64_t offset = sizeof(Math);
    for (const auto& layout : layouts) {
      offset += layout.bytes;
    }
    std::pmr::monotonic_buffer_resource arena(bytes + offset, arena_bytes,
                                              std::pmr::null_memory_resource());
    // Pooling recycles variable-size point/cut buffers within the admitted
    // arena.
    std::pmr::unsynchronized_pool_resource pool(&arena);
    PolygonArrangement polygon(m, &pool, max_events);
    auto quantization = m.exact.integer(0);
    const bool grid =
        kind == PolygonKind::Grid || kind == PolygonKind::GridFloat;
    for (unsigned port = 0; port < inputs.size(); ++port) {
      polygon.read(reader, port, grid, spacing, ox, oy, &quantization);
    }
    polygon.build(
        kind == PolygonKind::Publish || kind == PolygonKind::FillDistance
            ? BooleanKind::Union
            : operation_kind,
        evenodd_a, evenodd_b);
    if (kind == PolygonKind::FillDistance) {
      const auto& out = invocation.outputs[0];
      const bool narrow = canvas.type == ElementType::Float32;
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        if (polygon.boundary.empty()) {
          if (max_distance > 0) {
            store(out, at, m.rounded(m.value(max_distance), narrow));
          } else {
            no_solution("empty filled region has no signed distance");
          }
          return;
        }
        const ExactPoint point{m.center(canvas.origin_x, at[1]),
                               m.center(canvas.origin_y, at[0])};
        auto distance = polygon.squared_distance(point);
        if (max_distance > 0) {
          distance = m.min(distance, m.sq(m.value(max_distance)));
        }
        auto value = m.rounded_sqrt(distance, narrow);
        if (!Exact::zero(distance) && polygon.inside(point, 0, evenodd_a)) {
          value |= UINT64_C(1) << (narrow ? 31 : 63);
        }
        store(out, at, value);
      });
      return;
    }
    Dense out{layouts[0], bytes + sizeof(Math), work};
    Dense report{layouts[1], bytes + sizeof(Math) + layouts[0].bytes, work};
    out.clear();
    report.clear();
    auto publication = m.exact.integer(0), grid_rounding = m.exact.integer(0);
    std::uint64_t vertex = 0, contour = 0;
    CoreWriter writer(out, path_output);
    for (const auto& cycle : polygon.contours) {
      operational_capacity(contour < max_contours);
      if (kind == PolygonKind::Exact) {
        out.set(3, contour, vertex);
      } else if (kind == PolygonKind::Grid) {
        out.set(1, contour, vertex);
      } else {
        writer.begin(true);
      }
      for (auto j = cycle.begin; j < cycle.end; ++j) {
        operational_capacity(vertex < max_vertices);
        auto p = polygon.points[polygon.vertices[j]];
        if (grid) {
          const auto gx = round_integer(m, p.x), gy = round_integer(m, p.y);
          const ExactPoint q{m.exact.integer(gx), m.exact.integer(gy)};
          const auto delta = polygon.sub(q, p);
          grid_rounding =
              m.max(grid_rounding,
                    m.mul(m.sq(m.value(spacing)), polygon.dot(delta, delta)));
          p = q;
          if (kind == PolygonKind::Grid) {
            out.set(0, 2 * vertex, static_cast<std::uint64_t>(gx));
            out.set(0, 2 * vertex + 1, static_cast<std::uint64_t>(gy));
          } else {
            p.x = m.add(m.value(ox), m.mul(m.value(spacing), p.x));
            p.y = m.add(m.value(oy), m.mul(m.value(spacing), p.y));
          }
        }
        if (kind == PolygonKind::Exact) {
          write_rational(out, 0, 1, 2, vertex, 0, limbs, p.x);
          write_rational(out, 0, 1, 2, vertex, 1, limbs, p.y);
        } else if (kind != PolygonKind::Grid) {
          const auto x = m.rounded(p.x, false), y = m.rounded(p.y, false);
          const auto dx = m.sub(m.exact.binary(x), p.x),
                     dy = m.sub(m.exact.binary(y), p.y);
          publication = m.max(publication, m.add(m.sq(dx), m.sq(dy)));
          writer.point(j == cycle.begin ? 1 : 2, x, y);
        }
        ++vertex;
      }
      if (kind != PolygonKind::Exact && kind != PolygonKind::Grid) {
        writer.append(5, nullptr, 0);
      }
      ++contour;
    }
    if (kind == PolygonKind::Exact || kind == PolygonKind::Grid) {
      const unsigned offsets = kind == PolygonKind::Exact ? 3 : 1;
      const unsigned counts = kind == PolygonKind::Exact ? 4 : 2;
      for (auto c = contour; c <= max_contours; ++c) {
        out.set(offsets, c, vertex);
      }
      out.set(counts, 0, vertex);
      out.set(counts, 1, contour);
    } else {
      writer.finish();
    }
    // Sum of separately directed stage bounds. The grid algorithm retains
    // snapped cyclic edges, including degenerate contacts, without a reclip.
    double quantization_bound = 0;
    if (grid) {
      const auto q = m.add(m.value(upward_sqrt(m, quantization)),
                           m.value(upward_sqrt(m, grid_rounding)));
      quantization_bound = number(m.rounded(q, false));
      if (m.exact.order(m.value(quantization_bound), q) < 0) {
        quantization_bound = std::nextafter(
            quantization_bound, std::numeric_limits<double>::infinity());
      }
    }
    geometry_report(report, reader, inputs.size(), 0, quantization_bound,
                    upward_sqrt(m, publication));
    out.publish(kernel, invocation, 0);
    report.publish(kernel, invocation, 1);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_boolean(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    struct Entry {
      const char* key;
      PolygonKind kind;
      unsigned inputs;
    };
    const Entry entries[] = {
        {"path.boolean_polygon_regions_v1_strict", PolygonKind::Float, 2},
        {"path.boolean_exact_result_v1_strict", PolygonKind::Exact, 2},
        {"path.publish_exact_geometry_v1_strict", PolygonKind::Publish, 1},
        {"path.boolean_grid_result_v1_strict", PolygonKind::Grid, 2},
        {"path.boolean_grid_float64_v1_strict", PolygonKind::GridFloat, 2},
        {"path.fill_region_sdf_v1_strict", PolygonKind::FillDistance, 1}};
    for (const auto& entry : entries) {
      auto parameters = path_capacity_parameters();
      append(
          &parameters,
          {{"operation", OperationParameterType::String, false},
           {"fill_rule_a", OperationParameterType::String, false},
           {"fill_rule_b", OperationParameterType::String, false},
           {"max_vertices", OperationParameterType::Int64, false},
           {"max_contours", OperationParameterType::Int64, false},
           {"max_events", OperationParameterType::Int64, false},
           {"limb_capacity", OperationParameterType::Int64, false},
           {"max_scratch_bytes", OperationParameterType::Int64, false},
           {"epsilon_publication_px", OperationParameterType::Float64, false},
           {"grid_spacing", OperationParameterType::Float64, false},
           {"grid_origin_x", OperationParameterType::Float64, false},
           {"grid_origin_y", OperationParameterType::Float64, false}});
      std::vector<std::pair<std::string, SchemaTemplate>> outputs;
      if (entry.kind == PolygonKind::FillDistance) {
        append(&parameters, canvas_parameters());
        append(&parameters,
               {{"fill_rule", OperationParameterType::String, false},
                {"max_distance", OperationParameterType::Float64, false}});
        outputs.push_back(
            {"distance", tensor_schema(ElementType::Float64, {1, 1}, true)});
      } else {
        outputs.push_back(
            {entry.kind == PolygonKind::Exact || entry.kind == PolygonKind::Grid
                 ? "polygon"
                 : "path",
             entry.kind == PolygonKind::Exact
                 ? take(ps::generation::exact_polygon_schema(1, 1, 1))
             : entry.kind == PolygonKind::Grid
                 ? take(ps::generation::grid_polygon_schema(1, 1))
                 : take(ps::generation::core_path_schema({}))});
        outputs.push_back(
            {"report",
             take(ps::generation::geometry_report_schema(entry.inputs))});
      }
      const char* suffix[] = {"strict", "accelerated_apple_silicon",
                              "accelerated_x86_64"};
      for (unsigned profile = 0;
           profile < (entry.kind == PolygonKind::FillDistance ? 3U : 1U);
           ++profile) {
        const auto key =
            entry.kind == PolygonKind::FillDistance
                ? std::string("path.fill_region_sdf_v1_") + suffix[profile]
                : std::string(entry.key);
        auto definition = operation(
            key, entry.inputs, parameters, outputs,
            [kind = entry.kind, profile](const auto& in, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<PolygonProgram>(kind, in, p);
            },
            false, entry.kind == PolygonKind::FillDistance);
        for (unsigned port = 0; port < entry.inputs; ++port) {
          any_path_input(&definition, port);
        }
        check(registry->register_operation(std::move(definition)));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
