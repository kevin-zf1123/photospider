#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/circular_arrangement.hpp"
#include "03-generation/polygon_arrangement.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
using RPoint = RadicalPoint;
using Rid = Radical::Id;
struct StrokeRecipe {
  const Reader& reader;
  const PathInfo& info;
  CircularArrangement& geometry;
  Radical& r;
  RadicalPlane& p;
  bool variable;
  std::string cap, join;
  Rid radius, miter_limit;
  struct Segment {
    RPoint a, b, normal, tangent;
    Rid ra, rb;
  };
  std::pmr::vector<Segment> segments;
  StrokeRecipe(const Reader& input, const PathInfo& path,
               CircularArrangement& out, bool widths, const std::string& caps,
               const std::string& joins, double miter)
      : reader(input),
        info(path),
        geometry(out),
        r(out.r),
        p(out.p),
        variable(widths),
        cap(caps),
        join(joins),
        radius(0),
        miter_limit(r.value(miter)),
        segments(out.resource) {
    if (!variable) {
      radius = r.div(r.input(reader, 1, 0, {0}), r.integer(2));
    }
  }
  RPoint point(std::uint64_t control) {
    return {r.input(reader, 0, 2, {control, 0}),
            r.input(reader, 0, 2, {control, 1})};
  }
  Rid width(std::uint64_t control) {
    return variable ? r.div(r.input(reader, 1, 0, {control}), r.integer(2))
                    : radius;
  }
  void strip(const Segment& s) {
    const RPoint corners[] = {p.add(s.a, s.normal), p.sub(s.a, s.normal),
                              p.sub(s.b, s.normal), p.add(s.b, s.normal)};
    geometry.polygon(corners, 4);
  }
  void endpoint(const Segment& s, bool start) {
    const auto center = start ? s.a : s.b;
    const auto n = s.normal;
    if (cap == "round") {
      geometry.sector(center, radius, start ? n : p.mul(n, r.integer(-1)),
                      start ? p.mul(n, r.integer(-1)) : n);
    } else if (cap == "square") {
      const auto extension = p.mul(s.tangent, start ? r.neg(radius) : radius);
      const RPoint corners[] = {p.add(center, n), p.sub(center, n),
                                p.add(p.sub(center, n), extension),
                                p.add(p.add(center, n), extension)};
      geometry.polygon(corners, 4);
    }
  }
  void junction(const Segment& previous, const Segment& next) {
    const auto center = previous.b;
    const auto turn = r.sign(p.cross(previous.tangent, next.tangent));
    if (!turn) {
      if (join == "round" &&
          r.sign(p.dot(previous.tangent, next.tangent)) < 0) {
        geometry.sector(center, radius, p.mul(previous.normal, r.integer(-1)),
                        previous.normal);
      }
      return;
    }
    const auto side = r.integer(-turn);
    const auto a = p.add(center, p.mul(previous.normal, side));
    const auto b = p.add(center, p.mul(next.normal, side));
    if (join == "round") {
      const auto av = p.sub(a, center), bv = p.sub(b, center);
      geometry.sector(center, radius, turn > 0 ? av : bv, turn > 0 ? bv : av);
      return;
    }
    if (join == "miter") {
      const auto denominator = p.cross(previous.tangent, next.tangent);
      const auto t = r.div(p.cross(p.sub(b, a), next.tangent), denominator);
      const auto intersection = p.add(a, p.mul(previous.tangent, t));
      if (r.compare(p.norm(p.sub(intersection, center)),
                    r.sq(r.mul(radius, miter_limit))) <= 0) {
        const RPoint corners[] = {center, a, intersection, b};
        geometry.polygon(corners, 4);
        return;
      }
    }
    const RPoint corners[] = {center, a, b};
    geometry.polygon(corners, 3);
  }
  void build() {
    const auto count = path_counts(reader, 0, false);
    for (std::uint64_t subpath = 0; subpath < count[2]; ++subpath) {
      segments.clear();
      const auto begin = reader.index(0, 3, {subpath}),
                 end = reader.index(0, 3, {subpath + 1});
      const bool closed = reader.bits(0, 4, {subpath}) != 0;
      const auto first_control = static_cast<std::uint64_t>(
          reader.index(0, 1, {static_cast<std::uint64_t>(begin)}));
      auto previous_control = first_control;
      auto previous = point(first_control);
      for (auto row = begin + 1; row < end; ++row) {
        r.m.work.poll();
        const bool close =
            reader.bits(0, 0, {static_cast<std::uint64_t>(row)}) == 5;
        const auto control =
            close ? first_control
                  : static_cast<std::uint64_t>(
                        reader.index(0, 1, {static_cast<std::uint64_t>(row)}));
        const auto next = point(control);
        const auto ra = width(previous_control), rb = width(control);
        if (variable) {
          geometry.hull(previous, next, ra, rb);
        } else if (p.compare(previous, next)) {
          const auto v = p.sub(next, previous);
          const auto tangent = p.mul(v, r.inverse(r.sqrt(p.norm(v))));
          segments.push_back({previous, next, p.mul(p.perp(tangent), radius),
                              tangent, ra, rb});
        }
        previous = next;
        previous_control = control;
      }
      if (variable) {
        if (begin + 1 == end) {
          geometry.disk(previous, width(previous_control));
        }
        continue;
      }
      if (!r.sign(radius)) {
        continue;
      }
      if (segments.empty()) {
        if (cap == "round" || closed) {
          geometry.disk(previous, radius);
        } else if (cap == "square") {
          const RPoint corners[] = {
              {r.sub(previous.x, radius), r.sub(previous.y, radius)},
              {r.add(previous.x, radius), r.sub(previous.y, radius)},
              {r.add(previous.x, radius), r.add(previous.y, radius)},
              {r.sub(previous.x, radius), r.add(previous.y, radius)}};
          geometry.polygon(corners, 4);
        }
        continue;
      }
      // With round caps and joins the whole stroke is exactly a capsule union.
      if ((closed || cap == "round") && join == "round") {
        for (const auto& s : segments) {
          geometry.hull(s.a, s.b, radius, radius);
        }
        continue;
      }
      for (const auto& s : segments) {
        strip(s);
      }
      for (std::size_t j = 1; j < segments.size(); ++j) {
        junction(segments[j - 1], segments[j]);
      }
      if (closed) {
        junction(segments.back(), segments.front());
      } else {
        endpoint(segments.front(), true);
        endpoint(segments.back(), false);
      }
    }
  }
};
enum class StrokeKind { ConstantArea, VariableArea, Outline, Distance, Offset };
struct StrokeProgram final : Program {
  StrokeKind kind;
  PathInfo info, output_info;
  Canvas canvas;
  bool variable, strict_topology, evenodd;
  std::string cap, join;
  double miter, epsilon, maximum_distance;
  std::uint64_t arena_bytes, events, nodes, segments, refinements, depth;
  std::vector<DenseLayout> layouts;
  StrokeProgram(StrokeKind selected, const std::vector<OperationMetadata>& in,
                const Parameters& params)
      : kind(selected),
        info(path_info(*in[0].result_schema)),
        canvas(selected == StrokeKind::Outline || selected == StrokeKind::Offset
                   ? Parameters{{"height", std::int64_t{1}},
                                {"width", std::int64_t{1}}}
                   : params),
        variable(selected == StrokeKind::VariableArea ||
                 text(params, "width_mode", "constant") == "variable_round"),
        strict_topology(text(params, "topology_policy", "allow_change") ==
                        "reject_unproven"),
        evenodd(fill_evenodd(params)),
        cap(text(params, "cap", "round")),
        join(text(params, "join", "round")),
        miter(real(params, "miter_limit", 4)),
        epsilon(real(params, "epsilon_outline_px", 0.05)),
        maximum_distance(real(params, "max_distance")),
        arena_bytes(integer(params, "max_scratch_bytes", 128 * 1024 * 1024)),
        events(integer(params, "max_events", 1048576)),
        nodes(integer(params, "max_expression_nodes", 262144)),
        segments(integer(params, "max_segments", 1048576)),
        refinements(integer(params, "max_refinements", 4096)),
        depth(integer(params, "max_depth", 52)) {
    require(!info.primitive, "stroke and offset require CoreVerbs polylines");
    require(cap == "round" || cap == "butt" || cap == "square",
            "unknown stroke cap");
    require(join == "round" || join == "bevel" || join == "miter",
            "unknown stroke join");
    require(text(params, "width_mode", "constant") == "constant" ||
                text(params, "width_mode", "constant") == "variable_round",
            "unknown width recipe");
    require(text(params, "topology_policy", "allow_change") == "allow_change" ||
                strict_topology,
            "unknown outline topology policy");
    require(epsilon > 0 && miter >= 1 && events && nodes && segments &&
                refinements && depth <= 512,
            "invalid stroke geometry budgets");
    shape(in[1],
          {variable && kind != StrokeKind::Offset ? info.capacities[1] : 1});
    complete = kind == StrokeKind::Outline || kind == StrokeKind::Offset;
    if (complete) {
      output_info = output_core(params);
      add_output(this, path_schema(output_info));
      add_output(this, take(ps::generation::geometry_report_schema(2)));
      for (const auto& out : outputs) {
        layouts.emplace_back(*out.result_schema);
      }
    } else {
      add_output(this, tensor_schema(canvas.type, {canvas.height, canvas.width},
                                     true));
    }
    scratch_bytes = checked_add(sizeof(Math), arena_bytes);
    for (const auto& layout : layouts) {
      scratch_bytes = checked_add(scratch_bytes, layout.bytes);
    }
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arithmetic(base, work, refinements);
    auto& m = *arithmetic;
    std::uint64_t memory_offset = sizeof(Math);
    for (const auto& layout : layouts) {
      memory_offset += layout.bytes;
    }
    std::pmr::monotonic_buffer_resource arena(base + memory_offset, arena_bytes,
                                              std::pmr::null_memory_resource());
    std::pmr::unsynchronized_pool_resource pool(&arena);
    Radical r(m, &pool, nodes);
    CircularArrangement geometry(r, &pool, events);
    auto& p = geometry.p;
    if (kind == StrokeKind::Offset) {
      PolygonArrangement region(m, &pool, events);
      region.read(reader, 0);
      region.build(BooleanKind::Union, evenodd, false);
      geometry.offset_mode = true;
      const auto distance = r.input(reader, 1, 0, {0});
      geometry.erosion = r.sign(distance) < 0;
      geometry.offset_radius = r.absolute(distance);
      for (const auto& edge : region.boundary) {
        const auto& a = region.points[edge.a];
        const auto& b = region.points[edge.b];
        geometry.offset_edge({r.constant(a.x), r.constant(a.y)},
                             {r.constant(b.x), r.constant(b.y)},
                             geometry.offset_radius);
      }
    } else {
      StrokeRecipe recipe(reader, info, geometry, variable, cap, join, miter);
      recipe.build();
    }
    if (kind == StrokeKind::ConstantArea || kind == StrokeKind::VariableArea) {
      const auto saved = r.mark();
      const auto& output = invocation.outputs[0];
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        {
          CircularArrangement cell(r, &pool, events);
          cell.points = geometry.points;
          cell.point_order = geometry.point_order;
          cell.polygon_points = geometry.polygon_points;
          cell.shapes = geometry.shapes;
          cell.lines = geometry.lines;
          cell.circles = geometry.circles;
          cell.clip = true;
          const auto x = r.constant(
              m.add(m.exact.integer(canvas.origin_x), m.exact.integer(at[1])));
          const auto y = r.constant(
              m.add(m.exact.integer(canvas.origin_y), m.exact.integer(at[0])));
          cell.box = {x, y, r.add(x, 1), r.add(y, 1)};
          const RPoint corners[] = {{x, y},
                                    {cell.box[2], y},
                                    {cell.box[2], cell.box[3]},
                                    {x, cell.box[3]}};
          for (unsigned j = 0; j < 4; ++j) {
            cell.line(corners[j], corners[(j + 1) % 4]);
          }
          cell.arrange();
          store(output, at,
                cell.rounded_area(canvas.type == ElementType::Float32));
        }
        r.rewind(saved);
      });
      return;
    }
    geometry.arrange();
    if (kind == StrokeKind::Distance) {
      const auto saved = r.mark();
      const auto& output = invocation.outputs[0];
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        const bool narrow = canvas.type == ElementType::Float32;
        if (geometry.boundary.empty()) {
          if (maximum_distance <= 0) {
            no_solution("empty stroke has no signed boundary distance");
          }
          store(output, at, m.rounded(m.value(maximum_distance), narrow));
        } else {
          const RPoint query{r.constant(m.center(canvas.origin_x, at[1])),
                             r.constant(m.center(canvas.origin_y, at[0]))};
          auto distance = r.sqrt(geometry.distance_squared(query));
          if (maximum_distance > 0) {
            distance = r.min(distance, r.value(maximum_distance));
          }
          if (r.sign(distance) && geometry.contains({query, {0, 0}})) {
            distance = r.neg(distance);
          }
          store(output, at, r.round(distance, narrow));
        }
        r.rewind(saved);
      });
      return;
    }
    geometry.trace();
    const Dense path{layouts[0], base + sizeof(Math), work};
    const Dense report{layouts[1], base + sizeof(Math) + layouts[0].bytes,
                       work};
    path.clear();
    report.clear();
    CoreWriter writer(path, output_info);
    struct Piece {
      CircularArrangement::Edge edge;
      unsigned depth;
      std::size_t cycle;
      double deviation;
    };
    struct PublishedVertex {
      std::uint64_t x = 0, y = 0;
      double error = -1;
    };
    std::pmr::vector<Piece> leaves(&pool), refined(&pool), pending(&pool);
    std::pmr::vector<std::uint32_t> contour(&pool);
    std::pmr::vector<PublishedVertex> published_vertices(&pool);
    double approximation = 0, publication = 0;
    std::uint64_t used_segments = 0;
    const auto epsilon_value = m.value(epsilon);
    const auto fits = [&](double deviation) {
      const auto sum = m.add(m.value(deviation), m.value(publication));
      const auto order = m.exact.order(sum, epsilon_value);
      check(m.exact.status());
      return order <= 0;
    };
    const auto upward = [&](Rid value) {
      auto rounded = number(r.round(value));
      if (r.compare(r.value(rounded), value) < 0) {
        rounded =
            std::nextafter(rounded, std::numeric_limits<double>::infinity());
      }
      return rounded;
    };
    const auto publish_vertex = [&](std::uint32_t id) {
      if (published_vertices.size() < geometry.points.size()) {
        published_vertices.resize(geometry.points.size());
      }
      auto& value = published_vertices[id];
      if (value.error >= 0) {
        return;
      }
      const auto point = geometry.points[id];
      value.x = r.round(point.x);
      value.y = r.round(point.y);
      const RPoint rounded{r.constant(m.exact.binary(value.x)),
                           r.constant(m.exact.binary(value.y))};
      value.error = upward(r.sqrt(p.norm(p.sub(point, rounded))));
      publication = std::max(publication, value.error);
      // Every retained endpoint survives further subdivision.
      if (!fits(0)) {
        invalid_quality("outline endpoint publication exceeds geometric bound");
      }
    };
    const auto piece = [&](CircularArrangement::Edge edge, unsigned level,
                           std::size_t cycle) {
      double deviation = 0;
      if (edge.circle >= 0) {
        const auto a = geometry.points[edge.a], b = geometry.points[edge.b];
        const auto radius = geometry.circles[edge.circle].radius;
        const auto half_chord = r.div(p.norm(p.sub(b, a)), r.integer(4));
        // Arrangement cuts every circle at its four axis extrema.
        deviation =
            upward(r.sub(radius, r.sqrt(r.sub(r.sq(radius), half_chord))));
      }
      return Piece{edge, level, cycle, deviation};
    };
    // Only exposed endpoints contribute, including all straight-edge endpoints.
    for (const auto index : geometry.cycles) {
      work.poll();
      const auto edge = geometry.boundary[index];
      publish_vertex(edge.a);
      publish_vertex(edge.b);
    }
    for (std::size_t cycle = 0; cycle + 1 < geometry.cycle_offsets.size();
         ++cycle) {
      for (auto j = geometry.cycle_offsets[cycle];
           j < geometry.cycle_offsets[cycle + 1]; ++j) {
        operational_capacity(++used_segments <= segments);
        leaves.push_back(
            piece(geometry.boundary[geometry.cycles[j]], 0, cycle));
      }
    }
    for (;;) {
      approximation = 0;
      for (const auto& leaf : leaves) {
        work.poll();
        approximation = std::max(approximation, leaf.deviation);
      }
      if (fits(approximation)) {
        break;
      }
      refined.clear();
      for (const auto& leaf : leaves) {
        pending.push_back(leaf);
        while (!pending.empty()) {
          work.poll();
          const auto current = pending.back();
          pending.pop_back();
          if (fits(current.deviation)) {
            refined.push_back(current);
            continue;
          }
          if (current.edge.circle < 0 || current.depth >= depth) {
            invalid_quality(
                "outline subdivision depth cannot meet geometric bound");
          }
          operational_capacity(++used_segments <= segments);
          const auto edge = current.edge;
          const auto middle = geometry.vertex(geometry.arc_middle(edge));
          publish_vertex(middle);
          pending.push_back(piece({middle, edge.b, edge.circle, edge.ccw},
                                  current.depth + 1, current.cycle));
          pending.push_back(piece({edge.a, middle, edge.circle, edge.ccw},
                                  current.depth + 1, current.cycle));
        }
      }
      leaves.swap(refined);
      // A new endpoint can increase the global publication maximum. Revisit
      // earlier leaves against that new budget without discarding endpoints.
    }
    std::size_t cursor = 0;
    for (std::size_t cycle = 0; cycle + 1 < geometry.cycle_offsets.size();
         ++cycle) {
      contour.clear();
      while (cursor < leaves.size() && leaves[cursor].cycle == cycle) {
        contour.push_back(leaves[cursor++].edge.a);
      }
      if (contour.empty()) {
        continue;
      }
      // Rotate using mathematical vertices before the unchecked Float64
      // publish.
      std::size_t first = 0;
      const auto rotation_less = [&](std::size_t a, std::size_t b) {
        for (std::size_t j = 0; j < contour.size(); ++j) {
          const auto order =
              p.compare(geometry.points[contour[(a + j) % contour.size()]],
                        geometry.points[contour[(b + j) % contour.size()]]);
          if (order) {
            return order < 0;
          }
        }
        return false;
      };
      for (std::size_t j = 1; j < contour.size(); ++j) {
        if (rotation_less(j, first)) {
          first = j;
        }
      }
      writer.begin(true);
      for (std::size_t j = 0; j < contour.size(); ++j) {
        const auto value =
            published_vertices[contour[(first + j) % contour.size()]];
        writer.point(j ? 2 : 1, value.x, value.y);
      }
      writer.append(5, nullptr, 0);
    }
    writer.finish();
    const bool identity = approximation == 0 && publication == 0;
    if (strict_topology && !identity) {
      invalid_quality(
          "outline topology is not proved by the exact-identity certifier");
    }
    geometry_report(report, reader, 2, approximation, 0, publication,
                    identity ? 1 : 0);
    path.publish(kernel, invocation, 0);
    report.publish(kernel, invocation, 1);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_stroke(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    struct Entry {
      const char* key;
      StrokeKind kind;
      unsigned profiles;
    };
    const Entry entries[] = {
        {"path.stroke_constant_area_v1_", StrokeKind::ConstantArea, 3},
        {"path.stroke_variable_round_area_v1_", StrokeKind::VariableArea, 3},
        {"path.stroke_outline_flatten_v1_", StrokeKind::Outline, 1},
        {"path.stroke_region_sdf_v1_", StrokeKind::Distance, 3},
        {"path.offset_euclidean_region_v1_", StrokeKind::Offset, 1}};
    for (const auto& entry : entries) {
      for (unsigned profile = 0; profile < entry.profiles; ++profile) {
        auto parameters = path_capacity_parameters();
        if (entry.kind != StrokeKind::Outline &&
            entry.kind != StrokeKind::Offset) {
          append(&parameters, canvas_parameters());
        }
        append(
            &parameters,
            {{"cap", OperationParameterType::String, false},
             {"join", OperationParameterType::String, false},
             {"miter_limit", OperationParameterType::Float64, false},
             {"width_mode", OperationParameterType::String, false},
             {"fill_rule", OperationParameterType::String, false},
             {"topology_policy", OperationParameterType::String, false},
             {"epsilon_outline_px", OperationParameterType::Float64, false},
             {"epsilon_publication_px", OperationParameterType::Float64, false},
             {"max_distance", OperationParameterType::Float64, false},
             {"max_scratch_bytes", OperationParameterType::Int64, false},
             {"max_events", OperationParameterType::Int64, false},
             {"max_expression_nodes", OperationParameterType::Int64, false},
             {"max_segments", OperationParameterType::Int64, false},
             {"max_refinements", OperationParameterType::Int64, false},
             {"max_depth", OperationParameterType::Int64, false}});
        const bool complete = entry.kind == StrokeKind::Outline ||
                              entry.kind == StrokeKind::Offset;
        std::vector<std::pair<std::string, SchemaTemplate>> outputs;
        if (complete) {
          outputs = {
              {"path", take(ps::generation::core_path_schema({}))},
              {"report", take(ps::generation::geometry_report_schema(2))}};
        } else {
          outputs = {
              {entry.kind == StrokeKind::Distance ? "distance" : "coverage",
               tensor_schema(ElementType::Float64, {1, 1}, true)}};
        }
        auto op = operation(
            std::string(entry.key) + suffix[profile], 2, parameters, outputs,
            [kind = entry.kind, profile](const auto& in, const auto& params) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<StrokeProgram>(kind, in, params);
            },
            false, !complete);
        any_path_input(&op, 0);
        check(registry->register_operation(std::move(op)));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
