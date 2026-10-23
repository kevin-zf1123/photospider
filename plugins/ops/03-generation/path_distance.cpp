#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/algebraic.hpp"
#include "03-generation/path_support.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct ClosestCandidate final {
  AlgebraicRoot t, distance;
  std::array<Polynomial, 2> coordinate;
  std::int64_t segment = -1;
  std::uint64_t subpath = 0;
};
struct ClosestScratch final {
  EvaluationScratch evaluation;
  AlgebraicWorkspace algebra;
  ClosestCandidate best, candidate;
  std::array<AlgebraicRoot, 6> roots;
  AlgebraicRoot projected;
  Polynomial distance, stationary, relative, squared;
  std::array<Rational, 4> control;
  CurveValue first, last;
  explicit ClosestScratch(const Work& work) : evaluation(work) {}
};
struct CenterlineDistance final : Program {
  PathInfo info;
  Canvas canvas;
  double maximum;
  std::uint64_t refinements;
  CenterlineDistance(const std::vector<OperationMetadata>& in,
                     const Parameters& p)
      : info(path_info(*in[0].result_schema)),
        canvas(p),
        maximum(real(p, "max_distance")),
        refinements(integer(p, "max_refinements", 4096)) {
    require(refinements > 0,
            "positive closest-point refinement budget required");
    add_output(this,
               tensor_schema(canvas.type, {canvas.height, canvas.width}, true));
    add_output(this,
               tensor_schema(ElementType::Float64,
                             {canvas.height, canvas.width, 2}, true, true));
    add_output(this, tensor_schema(ElementType::Int64,
                                   {canvas.height, canvas.width}, true));
    add_output(this, tensor_schema(ElementType::Float64,
                                   {canvas.height, canvas.width}, true));
    scratch_bytes = sizeof(ClosestScratch);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* arena = new (invocation.scratch[0].allocation) ClosestScratch(work);
    struct Destroy {
      ClosestScratch* p;
      ~Destroy() { p->~ClosestScratch(); }
    } destroy{arena};
    auto& m = arena->evaluation.math;
    m.max_refinements = refinements;
    Algebra algebra(m, arena->algebra);
    PathCursor path(reader, info, m);
    CurveMath curves(reader, arena->evaluation);
    const auto& out = invocation.outputs[0];
    const unsigned port = kernel.writes[0].first;
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      m.exact.reset();
      const std::array<Rational, 2> query{m.center(canvas.origin_x, at[1]),
                                          m.center(canvas.origin_y, at[0])};
      bool found = false;
      auto& candidate = arena->candidate;
      auto& best = arena->best;
      const auto consider = [&] {
        const int order =
            found ? algebra.compare(&candidate.distance, &best.distance) : -1;
        bool precedes = candidate.subpath < best.subpath ||
                        (candidate.subpath == best.subpath &&
                         candidate.segment < best.segment);
        if (found && !order && candidate.subpath == best.subpath &&
            candidate.segment == best.segment)
          precedes = algebra.compare(&candidate.t, &best.t) < 0;
        if (!found || order < 0 || (!order && precedes)) {
          best = candidate;
          found = true;
        }
      };
      for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
        candidate.subpath = s;
        if (path.first(s) == path.end(s)) {
          candidate.segment = -1;
          algebra.rational(&candidate.t, m.exact.integer(0));
          Rational distance;
          for (unsigned axis = 0; axis < 2; ++axis) {
            const auto value = m.exact.binary(path.isolated(s, axis));
            algebra.constant(&candidate.coordinate[axis], value);
            distance = m.add(distance, m.sq(m.sub(value, query[axis])));
          }
          algebra.rational(&candidate.distance, distance);
          consider();
          continue;
        }
        for (auto row = path.first(s); row < path.end(s); ++row) {
          work.poll();
          const auto curve = path.curve(row);
          candidate.segment = row;
          for (unsigned axis = 0; axis < 2; ++axis) {
            auto& polynomial = candidate.coordinate[axis];
            algebra.clear(&polynomial);
            polynomial.degree =
                curve.kind == CurveKind::Close ? 1 : curve.degree;
            auto& control = arena->control;
            if (curve.kind == CurveKind::Close) {
              auto& first = arena->first;
              auto& last = arena->last;
              curves.rational(curve, m.exact.integer(0), false, &first);
              curves.rational(curve, m.exact.integer(1), false, &last);
              control[0] = first.position[axis];
              control[1] = last.position[axis];
            } else {
              for (unsigned j = 0; j <= curve.degree; ++j)
                control[j] = curve.core ? m.input(reader, 0, curve.field,
                                                  {curve.control[j], axis})
                                        : m.input(reader, 0, curve.field,
                                                  {curve.row, j, axis});
            }
            unsigned choose = 1;
            for (int power = 0; power <= polynomial.degree; ++power) {
              polynomial.c[power] = m.scale(control[0], choose);
              for (int j = 0; j < polynomial.degree - power; ++j)
                control[j] = m.sub(control[j + 1], control[j]);
              if (power < polynomial.degree)
                choose = choose * (polynomial.degree - power) / (power + 1);
            }
            algebra.trim(&polynomial);
          }
          auto& distance = arena->distance;
          algebra.clear(&distance);
          for (unsigned axis = 0; axis < 2; ++axis) {
            auto& relative = arena->relative;
            relative = candidate.coordinate[axis];
            relative.c[0] = m.sub(relative.c[0], query[axis]);
            relative.degree = std::max(0, relative.degree);
            algebra.trim(&relative);
            algebra.multiply(&arena->squared, relative, relative);
            algebra.add(&distance, distance, arena->squared);
          }
          auto& stationary = arena->stationary;
          algebra.derivative(&stationary, distance);
          // Endpoints and all real stationary roots are exhaustive even for
          // repeated roots, zero derivatives and self-intersecting Beziers.
          const unsigned count =
              algebra.roots(stationary, m.exact.integer(0), m.exact.integer(1),
                            &arena->roots);
          for (unsigned j = 0; j < count + 2; ++j) {
            if (j < 2)
              algebra.rational(&candidate.t, m.exact.integer(j));
            else
              candidate.t = arena->roots[j - 2];
            algebra.project(&candidate.distance, &candidate.t, distance);
            consider();
          }
        }
      }
      if (!found) {
        if (port == 0 && maximum > 0) {
          store(
              out, at,
              m.rounded(m.value(maximum), canvas.type == ElementType::Float32));
          return;
        }
        no_solution("empty path has no closest point");
      }
      if (port == 0) {
        if (maximum > 0 &&
            algebra.compare(&best.distance, m.sq(m.value(maximum))) > 0)
          store(
              out, at,
              m.rounded(m.value(maximum), canvas.type == ElementType::Float32));
        else
          store(out, at,
                algebra.round(&best.distance,
                              canvas.type == ElementType::Float32, true));
      } else if (port == 1) {
        algebra.project(&arena->projected, &best.t, best.coordinate[at[2]]);
        store(out, at, algebra.round(&arena->projected, false));
      } else if (port == 2) {
        store(out, at, static_cast<std::uint64_t>(best.segment));
      } else {
        store(out, at, algebra.round(&best.t, false));
      }
      check(m.exact.status());
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_centerline_distance(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    auto p = canvas_parameters();
    append(&p, {{"max_distance", OperationParameterType::Float64, false},
                {"max_refinements", OperationParameterType::Int64, false}});
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned profile = 0; profile < 3; ++profile) {
      auto op = operation(
          std::string("path.centerline_distance_v1_") + suffix[profile], 1, p,
          {{"distance", tensor_schema(ElementType::Float64, {1, 1}, true)},
           {"closest",
            tensor_schema(ElementType::Float64, {1, 1, 2}, true, true)},
           {"segment_id", tensor_schema(ElementType::Int64, {1, 1}, true)},
           {"local_t", tensor_schema(ElementType::Float64, {1, 1}, true)}},
          [profile](const auto& in, const auto& parameters) {
            check(numeric_ops::sequence_profile_available(
                static_cast<numeric_ops::SequenceProfile>(profile)));
            return std::make_shared<CenterlineDistance>(in, parameters);
          });
      any_path_input(&op, 0);
      check(registry->register_operation(std::move(op)));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
