#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/curve_integral.hpp"
#include "common/registry.hpp"
#include "photospider/ops/generation.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct PathLength final : Program {
  PathInfo info;
  std::uint64_t max_rows, panel_limit, max_refinements;
  unsigned depth;
  double epsilon;
  std::vector<DenseLayout> layouts;
  PathLength(const std::vector<OperationMetadata>& in, const Parameters& p,
             bool primitive)
      : info(path_info(*in[0].result_schema)),
        max_rows(integer(p, "max_table_rows", 4096)),
        panel_limit(integer(p, "max_integration_panels", 1048576)),
        max_refinements(integer(p, "max_refinements", 4096)),
        depth(integer(p, "max_depth", 24)),
        epsilon(real(p, "epsilon_length_px", 1e-6)) {
    require(info.primitive == primitive,
            primitive ? "primitive length requires Primitives"
                      : "Bezier length requires CoreVerbs");
    require(epsilon > 0 && max_rows && panel_limit && max_refinements &&
                depth <= 52,
            "invalid length integration budgets");
    add_output(this, tensor_schema(ElementType::Float64, {1}));
    add_output(this,
               take(ps::generation::path_arclength_schema(
                   info.capacities[0], max_rows, subpath_capacity(info))));
    scratch_bytes = sizeof(CurveIntegralScratch);
    scratch_bytes = checked_add(
        scratch_bytes, (subpath_capacity(info) + 1) * sizeof(OwnedInterval));
    for (const auto& output : outputs) {
      layouts.emplace_back(*output.result_schema);
      scratch_bytes = checked_add(scratch_bytes, layouts.back().bytes);
    }
    complete = true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    auto* storage = new (base) CurveIntegralScratch(work);
    struct Destroy {
      CurveIntegralScratch* p;
      ~Destroy() { p->~CurveIntegralScratch(); }
    } destroy{storage};
    base += sizeof(CurveIntegralScratch);
    auto* totals = reinterpret_cast<OwnedInterval*>(base);
    for (std::uint64_t j = 0; j <= subpath_capacity(info); ++j)
      new (totals + j) OwnedInterval{};
    base += (subpath_capacity(info) + 1) * sizeof(OwnedInterval);
    const Dense length{layouts[0], base, work};
    base += layouts[0].bytes;
    const Dense table{layouts[1], base, work};
    length.clear();
    table.clear();
    CurveIntegral integral(reader, *storage, panel_limit);
    auto& m = integral.m;
    auto& im = integral.im;
    PathCursor path(reader, info, m);
    std::uint64_t segments = 0;
    for (std::uint64_t s = 0; s < path.subpaths(); ++s)
      segments += path.end(s) - path.first(s);
    bool certified = false;
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < max_refinements;
         precision *= 2, ++attempt) {
      m.d.reset();
      m.d.precision = precision;
      integral.panels_used = 0;
      bool ready = true;
      try {
        totals[0] = im.integer(0);
        for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
          auto& subtotal = totals[s + 1];
          subtotal = im.integer(0);
          for (auto row = path.first(s); row < path.end(s); ++row) {
            const auto value = integral.segment(
                path.curve(row), m.exact.integer(0), m.exact.integer(1),
                m.exact.integer(0), kIntegralDepth, precision / 2);
            subtotal = im.add(subtotal, value);
          }
          totals[0] = im.add(totals[0], subtotal);
          std::uint64_t rounded = 0;
          if (!im.rounded(subtotal, false, &rounded))
            ready = false;
          table.set(4, (s + 1) * 3, rounded);
        }
        std::uint64_t rounded = 0;
        if (!im.rounded(totals[0], false, &rounded))
          ready = false;
        if (ready) {
          table.set(4, 0, rounded);
          length.set(0, 0, rounded);
          certified = true;
          break;
        }
      } catch (const IntegrationRetry&) {
        continue;
      }
    }
    if (!certified)
      throw Status{ErrorCode::OperationFailed,
                   "true path length rounding unresolved",
                   DomainErrorCode{UINT64_C(0x47454e0001)}};
    for (std::uint64_t s = 0; s <= path.subpaths(); ++s) {
      table.set(4, s * 3 + 1, im.endpoint(totals[s].low, false));
      table.set(4, s * 3 + 2, im.endpoint(totals[s].high, true));
    }
    std::uint64_t segment = 0, knot = 0;
    auto cumulative = im.integer(0);
    const auto append_knot = [&](const Rational& t) {
      operational_capacity(knot < max_rows);
      table.set(2, knot * 3, m.rounded(t, false));
      table.set(2, knot * 3 + 1, im.endpoint(cumulative.low, false));
      table.set(2, knot * 3 + 2, im.endpoint(cumulative.high, true));
      ++knot;
    };
    integral.panels_used = 0;
    const auto allowance = m.div(
        m.value(epsilon), m.exact.integer(std::max(UINT64_C(1), segments)));
    try {
      for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
        table.set(3, s, segment);
        for (auto row = path.first(s); row < path.end(s); ++row) {
          table.set(0, segment, row);
          table.set(1, segment, knot);
          append_knot(m.exact.integer(0));
          const auto curve = path.curve(row);
          if (curve.kind == CurveKind::Bezier ||
              curve.kind == CurveKind::Hermite) {
            integral.initialize(curve);
            integral.integrate_piece(
                m.exact.integer(0), m.exact.integer(1), allowance, depth,
                [&](const Rational&, const Rational& end,
                    const OwnedInterval& value) {
                  cumulative = im.add(cumulative, value);
                  append_knot(end);
                },
                true);
          } else {
            // The primitive integrator splits actual knot spans internally.
            // Only exactly represented source parameters are published.
            const auto value =
                integral.segment(curve, m.exact.integer(0), m.exact.integer(1),
                                 allowance, depth);
            cumulative = im.add(cumulative, value);
            append_knot(m.exact.integer(1));
          }
          ++segment;
        }
      }
    } catch (const IntegrationRetry&) {
      throw Status{ErrorCode::OperationFailed,
                   "length table enclosure unresolved",
                   DomainErrorCode{UINT64_C(0x47454e0001)}};
    }
    for (auto row = segment; row <= info.capacities[0]; ++row)
      table.set(1, row, knot);
    for (auto s = path.subpaths(); s <= subpath_capacity(info); ++s)
      table.set(3, s, segment);
    table.set(5, 0, segment);
    table.set(5, 1, knot);
    table.set(5, 2, path.subpaths());
    write_id(table, 6, 0, source_id(reader, 0));
    length.publish(kernel, invocation, 0);
    table.publish(kernel, invocation, 1);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_length(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (bool primitive : {false, true})
      for (unsigned profile = 0; profile < 3; ++profile) {
        auto op = operation(
            std::string(primitive ? "path.primitive_arc_length_v1_"
                                  : "path.bezier_arc_length_v1_") +
                suffix[profile],
            1,
            {{"epsilon_length_px", OperationParameterType::Float64, false},
             {"max_depth", OperationParameterType::Int64, false},
             {"max_table_rows", OperationParameterType::Int64, false},
             {"max_integration_panels", OperationParameterType::Int64, false},
             {"max_refinements", OperationParameterType::Int64, false}},
            {{"length", tensor_schema(ElementType::Float64, {1})},
             {"table", take(ps::generation::path_arclength_schema(1, 2, 1))}},
            [primitive, profile](const auto& in, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<PathLength>(in, p, primitive);
            },
            false, false);
        any_path_input(&op, 0);
        check(registry->register_operation(std::move(op)));
      }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
