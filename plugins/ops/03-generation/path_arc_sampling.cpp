#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_arc.hpp"
#include "common/registry.hpp"
#include "photospider/ops/generation.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct ArcSampling final : Program {
  PathInfo info;
  bool spacing, include_end;
  std::uint64_t count_per_subpath, maximum, panel_limit, refinements,
      cache_count;
  DenseLayout layout{SchemaTemplate{}};
  ArcSampling(const std::vector<OperationMetadata>& in, const Parameters& p,
              bool by_spacing)
      : info(path_info(*in[0].result_schema)),
        spacing(by_spacing),
        include_end(p.count("include_open_end")
                        ? std::get<bool>(p.at("include_open_end"))
                        : true),
        count_per_subpath(integer(p, "count_per_subpath", 2)),
        maximum(integer(p, "max_count",
                        by_spacing ? 4096
                                   : size_product({subpath_capacity(info),
                                                   count_per_subpath}))),
        panel_limit(integer(p, "max_integration_panels", 1048576)),
        refinements(integer(p, "max_refinements", 4096)),
        cache_count(2 * (info.capacities[0] + 1) + subpath_capacity(info)) {
    require(count_per_subpath && maximum && maximum <= (UINT64_C(1) << 40) &&
                panel_limit && refinements,
            "invalid arc sample budgets");
    if (spacing) {
      shape(in[1], {1});
    }
    add_output(this, take(ps::generation::path_samples_schema(
                         maximum, subpath_capacity(info), true)));
    layout = DenseLayout(*outputs[0].result_schema);
    scratch_bytes = checked_add(
        sizeof(CurveIntegralScratch),
        checked_add(cache_count * sizeof(OwnedInterval), layout.bytes));
    complete = true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    auto* arena = new (base) CurveIntegralScratch(work);
    struct Destroy {
      CurveIntegralScratch* p;
      ~Destroy() { p->~CurveIntegralScratch(); }
    } destroy{arena};
    base += sizeof(CurveIntegralScratch);
    auto* cache = reinterpret_cast<OwnedInterval*>(base);
    for (std::uint64_t j = 0; j < cache_count; ++j) {
      new (cache + j) OwnedInterval{};
    }
    base += cache_count * sizeof(OwnedInterval);
    const Dense out{layout, base, work};
    CurveIntegral integral(reader, *arena, panel_limit);
    integral.m.max_refinements = refinements;
    auto& m = integral.m;
    auto& im = integral.im;
    ArcTraversal arc(integral, info, cache, cache + info.capacities[0] + 1,
                     cache + 2 * (info.capacities[0] + 1));
    const auto delta =
        spacing ? m.input(reader, 1, 0, {0}) : m.exact.integer(1);
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < refinements;
         precision *= 2, ++attempt) {
      m.d.reset();
      m.d.precision = precision;
      out.clear();
      try {
        arc.index();
        std::uint64_t output = 0;
        for (std::uint64_t s = 0; s < arc.path.subpaths(); ++s) {
          out.set(3, s, output);
          const bool closed = arc.path.closed(s);
          const bool zero = arc.zero(arc.totals[s]);
          std::uint64_t samples_count = count_per_subpath;
          std::uint64_t regular_count = 0;
          if (spacing) {
            if (zero) {
              samples_count = 1;
            } else {
              const auto ratio = im.div(arc.totals[s], im.rational(delta));
              const auto ceiling = [&](const Directed::Number& value) {
                std::uint64_t lo = 0, hi = maximum + 1;
                while (lo < hi) {
                  work.poll();
                  const auto mid = lo + (hi - lo) / 2;
                  const auto integer_bound = im.integer(mid);
                  if (m.d.compare(value, integer_bound.low) <= 0) {
                    hi = mid;
                  } else {
                    lo = mid + 1;
                  }
                }
                return lo;
              };
              const auto lower = ceiling(ratio.low),
                         upper = ceiling(ratio.high);
              operational_capacity(lower <= maximum);
              if (lower != upper) {
                const auto candidate = m.mul(m.exact.integer(lower), delta);
                if (arc.compare_target(s, arc.path.end(s),
                                       im.rational(candidate), arc.totals[s],
                                       nullptr, &candidate) != 0) {
                  throw IntegrationRetry{};
                }
              }
              regular_count = lower;
              samples_count = regular_count + (!closed && include_end);
            }
          }
          operational_capacity(samples_count <= maximum - output);
          for (std::uint64_t j = 0; j < samples_count; ++j) {
            work.poll();
            const bool end = !zero && !closed &&
                             (spacing ? include_end && j == regular_count
                                      : count_per_subpath >= 2 &&
                                            j + 1 == count_per_subpath);
            const auto fraction = m.exact.fraction(
                j, closed ? count_per_subpath
                          : std::max(UINT64_C(1), count_per_subpath - 1));
            const auto absolute = m.mul(m.exact.integer(j), delta);
            const auto target =
                zero      ? im.integer(0)
                : end     ? arc.totals[s]
                : spacing ? im.rational(absolute)
                          : im.mul(arc.totals[s], im.rational(fraction));
            const auto location = arc.locate(s, target, j == 0 || zero, end,
                                             spacing ? nullptr : &fraction,
                                             spacing ? &absolute : nullptr);
            const auto values = arc.rounded_location(s, location);
            std::uint64_t arc_s = 0;
            if (!im.rounded(target, false, &arc_s)) {
              Rational exact_arc;
              const auto full = m.exact.integer(1);
              if (!arc.exact_ratio(s, arc.path.first(s),
                                   end       ? &full
                                   : spacing ? nullptr
                                             : &fraction,
                                   spacing && !end ? &absolute : nullptr,
                                   m.exact.integer(1), false, &exact_arc)) {
                throw IntegrationRetry{};
              }
              arc_s = m.rounded(exact_arc, false);
            }
            out.set(0, output * 2, values[0]);
            out.set(0, output * 2 + 1, values[1]);
            out.set(1, output, location.segment);
            out.set(2, output, values[2]);
            out.set(6, output, arc_s);
            ++output;
          }
        }
        for (auto s = arc.path.subpaths(); s <= subpath_capacity(info); ++s) {
          out.set(3, s, output);
        }
        out.set(4, 0, output);
        out.set(4, 1, arc.path.subpaths());
        write_id(out, 5, 0, source_id(reader, 0));
        out.publish(kernel, invocation);
        return;
      } catch (const IntegrationRetry&) {
        continue;
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "arc sampling certification unresolved",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_arc_sampling(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    for (bool spacing : {false, true}) {
      auto op = operation(
          spacing ? "path.resample_arc_spacing_v1_strict"
                  : "path.resample_uniform_arc_v1_strict",
          spacing ? 2 : 1,
          {{"count_per_subpath", OperationParameterType::Int64, false},
           {"max_count", OperationParameterType::Int64, false},
           {"include_open_end", OperationParameterType::Bool, false},
           {"max_integration_panels", OperationParameterType::Int64, false},
           {"max_refinements", OperationParameterType::Int64, false}},
          {{"samples", take(ps::generation::path_samples_schema(1, 1, true))}},
          [spacing](const auto& in, const auto& p) {
            return std::make_shared<ArcSampling>(in, p, spacing);
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
