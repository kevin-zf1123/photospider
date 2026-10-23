#include <array>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_arc.hpp"
#include "03-generation/polygon_arrangement.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct TrimScratch {
  CurveIntegralScratch integral;
  std::array<std::array<Rational, 2>, 4> exact;
  std::array<std::array<OwnedInterval, 2>, 4> interval;
  explicit TrimScratch(const Work& work) : integral(work) {}
};
struct ArcRange {
  Rational a, b;
  bool end = false;
};
struct TrimProgram final : Program {
  PathInfo info, output;
  bool dash, normalized, wrap, drop;
  std::uint64_t pattern_count, maximum, panels, refinements, limbs, cache_count,
      arena_bytes;
  std::vector<DenseLayout> layouts;
  TrimProgram(const std::vector<OperationMetadata>& in,
              const Parameters& params, bool dashed)
      : info(path_info(*in[0].result_schema)),
        output(output_core(params)),
        dash(dashed),
        normalized(text(params, "units", "normalized_arc") == "normalized_arc"),
        wrap(params.count("closed_wrap")
                 ? std::get<bool>(params.at("closed_wrap"))
                 : false),
        drop(text(params, "attribute_policy", "reject_unmappable") ==
             "drop_unmappable"),
        pattern_count(0),
        maximum(integer(params, "max_output_segments", 4096)),
        panels(integer(params, "max_integration_panels", 1048576)),
        refinements(integer(params, "max_refinements", 4096)),
        limbs(integer(params, "limb_capacity", 128)),
        cache_count(2 * (info.capacities[0] + 1) + subpath_capacity(info)),
        arena_bytes(integer(params, "max_scratch_bytes", 64 * 1024 * 1024)) {
    require(!info.primitive,
            "trim and dash require CoreVerbs Bezier authority");
    require(text(params, "units", "normalized_arc") == "arc_px" || normalized,
            "unknown trim units");
    require(text(params, "attribute_policy", "reject_unmappable") ==
                    "reject_unmappable" ||
                drop,
            "unknown attribute policy");
    require(text(params, "restart", "per_subpath") == "per_subpath",
            "unknown dash restart");
    require(maximum && panels && refinements && limbs, "invalid trim budgets");
    shape(in[2], {1});
    if (dash) {
      const auto dimensions = tensor(in[1]).sample_shape();
      require(dimensions.size() == 1 && dimensions[0] > 0,
              "dash pattern requires K>=1");
      pattern_count = dimensions[0];
      shape(in[1], {pattern_count});
    } else {
      shape(in[1], {1});
    }
    add_output(this, path_schema(output));
    add_output(this,
               take(ps::generation::path_source_map_schema(maximum, 1, limbs)));
    add_output(this, take(ps::generation::geometry_report_schema(1)));
    scratch_bytes = checked_add(
        sizeof(TrimScratch),
        checked_add(cache_count * sizeof(OwnedInterval), arena_bytes));
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
    auto* storage = new (base) TrimScratch(work);
    struct Destroy {
      TrimScratch* p;
      ~Destroy() { p->~TrimScratch(); }
    } destroy{storage};
    base += sizeof(TrimScratch);
    auto* cache = reinterpret_cast<OwnedInterval*>(base);
    for (std::uint64_t j = 0; j < cache_count; ++j) {
      new (cache + j) OwnedInterval{};
    }
    base += cache_count * sizeof(OwnedInterval);
    const Dense path{layouts[0], base, work};
    base += layouts[0].bytes;
    const Dense mapping{layouts[1], base, work};
    base += layouts[1].bytes;
    const Dense report{layouts[2], base, work};
    base += layouts[2].bytes;
    std::pmr::monotonic_buffer_resource memory(
        base, arena_bytes, std::pmr::null_memory_resource());
    std::pmr::unsynchronized_pool_resource pool(&memory);
    std::pmr::vector<Rational> pattern(&pool);
    std::pmr::vector<ArcRange> ranges(&pool);
    CurveIntegral integral(reader, storage->integral, panels);
    auto& m = integral.m;
    auto& im = integral.im;
    m.max_refinements = refinements;
    ArcTraversal arc(integral, info, cache, cache + info.capacities[0] + 1,
                     cache + 2 * (info.capacities[0] + 1));
    const auto first_value =
        dash ? m.exact.integer(0) : m.input(reader, 1, 0, {0});
    const auto second_value = m.input(reader, 2, 0, {0});
    auto phase = m.exact.integer(0);
    if (dash) {
      auto period = m.exact.integer(0);
      for (std::uint64_t j = 0; j < pattern_count * (1 + pattern_count % 2);
           ++j) {
        pattern.push_back(m.input(reader, 1, 0, {j % pattern_count}));
        period = m.add(period, pattern.back());
      }
      phase = m.mul(period, m.exact.modulo(m.div(second_value, period), 1));
    }
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < refinements;
         precision *= 2, ++attempt) {
      m.d.reset();
      m.d.precision = precision;
      path.clear();
      mapping.clear();
      report.clear();
      CoreWriter writer(path, output);
      std::uint64_t mappings = 0, emitted = 0;
      bool unmapped = false;
      try {
        arc.index();
        const auto map = [&](std::uint64_t target, std::uint64_t s,
                             std::uint64_t row, const ArcLocation& a,
                             const ArcLocation& b) {
          operational_capacity(++emitted <= maximum);
          if (!a.exact || !b.exact) {
            if (!drop) {
              invalid_quality(
                  "non-rational true-arc cut is not representable by "
                  "path_source_map v1");
            }
            unmapped = true;
            return;
          }
          mapping.set(0, 4 * mappings, target);
          mapping.set(0, 4 * mappings + 2, s);
          mapping.set(0, 4 * mappings + 3, row);
          write_rational(mapping, 2, 3, 4, mappings, 0, limbs, a.low);
          write_rational(mapping, 2, 3, 4, mappings, 1, limbs,
                         m.sub(b.low, a.low));
          ++mappings;
        };
        const auto copy_subpath = [&](std::uint64_t s) {
          writer.begin(arc.path.closed(s));
          for (auto row = arc.path.begin(s); row < arc.path.end(s); ++row) {
            const auto verb = reader.bits(0, 0, {row});
            const auto begin = reader.index(0, 1, {row}),
                       end = reader.index(0, 1, {row + 1});
            std::uint64_t controls[6]{};
            for (auto j = begin; j < end; ++j) {
              for (unsigned axis = 0; axis < 2; ++axis) {
                controls[2 * (j - begin) + axis] =
                    reader.bits(0, 2, {static_cast<std::uint64_t>(j), axis});
              }
            }
            const auto target = writer.verbs;
            writer.append(static_cast<unsigned>(verb), controls,
                          static_cast<unsigned>(end - begin));
            if (row != arc.path.begin(s)) {
              map(target, s, row, arc.endpoint(row, 0), arc.endpoint(row, 1));
            }
          }
        };
        const auto emit = [&](std::uint64_t s, const OwnedInterval& begin,
                              const OwnedInterval& end, bool start_endpoint,
                              bool end_endpoint, const Rational* norm_a,
                              const Rational* norm_b, bool new_subpath,
                              const Rational* abs_a = nullptr,
                              const Rational* abs_b = nullptr) {
          const auto start =
              arc.locate(s, begin, start_endpoint, false, norm_a, abs_a);
          const auto finish =
              arc.locate(s, end, false, end_endpoint, norm_b, abs_b);
          if (start.segment < 0) {
            return;
          }
          if (new_subpath) {
            const auto point = arc.rounded_location(s, start);
            writer.begin(false);
            writer.point(1, point[0], point[1]);
          }
          for (auto row = start.segment; row <= finish.segment; ++row) {
            const auto a = row == start.segment ? start : arc.endpoint(row, 0);
            const auto b =
                row == finish.segment ? finish : arc.endpoint(row, 1);
            if (a.exact && b.exact && m.exact.order(a.low, b.low) == 0) {
              continue;
            }
            if ((!a.exact || !b.exact) && !drop) {
              invalid_quality(
                  "non-rational true-arc cut is not representable by "
                  "path_source_map v1");
            }
            const auto curve = arc.path.curve(row);
            std::uint64_t controls[6]{};
            const auto aa = im.range(a.low, a.high),
                       bb = im.range(b.low, b.high);
            for (unsigned result = 1; result <= curve.degree; ++result) {
              for (unsigned j = 0; j <= curve.degree; ++j) {
                for (unsigned axis = 0; axis < 2; ++axis) {
                  storage->exact[j][axis] =
                      m.input(reader, 0, 2, {curve.control[j], axis});
                  storage->interval[j][axis] =
                      im.rational(storage->exact[j][axis]);
                }
              }
              for (unsigned level = 0; level < curve.degree; ++level) {
                const bool right = level >= curve.degree - result;
                const auto& t = right ? b.low : a.low;
                const auto& bound = right ? bb : aa;
                for (unsigned j = 0; j < curve.degree - level; ++j) {
                  for (unsigned axis = 0; axis < 2; ++axis) {
                    if (a.exact && b.exact) {
                      storage->exact[j][axis] =
                          m.lerp(storage->exact[j][axis],
                                 storage->exact[j + 1][axis], t);
                    } else {
                      storage->interval[j][axis] = im.add(
                          storage->interval[j][axis],
                          im.mul(bound, im.sub(storage->interval[j + 1][axis],
                                               storage->interval[j][axis])));
                    }
                  }
                }
              }
              for (unsigned axis = 0; axis < 2; ++axis) {
                auto& value = controls[2 * (result - 1) + axis];
                if (a.exact && b.exact) {
                  value = m.rounded(storage->exact[0][axis], false);
                } else if (!im.rounded(storage->interval[0][axis], false,
                                       &value)) {
                  throw IntegrationRetry{};
                }
              }
            }
            const auto target = writer.verbs;
            writer.append(curve.degree == 1   ? 2
                          : curve.degree == 2 ? 3
                                              : 4,
                          controls, curve.degree);
            map(target, s, row, a, b);
          }
        };
        for (std::uint64_t s = 0; s < arc.path.subpaths(); ++s) {
          if (arc.zero(arc.totals[s])) {
            continue;
          }
          if (!dash) {
            const int order = m.exact.order(first_value, second_value);
            if (!order) {
              continue;
            }
            const auto a = normalized
                               ? im.mul(arc.totals[s], im.rational(first_value))
                               : im.rational(first_value);
            const auto b =
                normalized ? im.mul(arc.totals[s], im.rational(second_value))
                           : im.rational(second_value);
            const bool start = Exact::zero(first_value);
            int end_order =
                normalized
                    ? m.exact.order(second_value, m.exact.integer(1))
                    : arc.compare_target(s, arc.path.end(s), b, arc.totals[s],
                                         nullptr, &second_value);
            if (end_order == 2) {
              throw IntegrationRetry{};
            }
            const bool finish = end_order == 0;
            if (start && finish) {
              copy_subpath(s);
              continue;
            }
            if (order > 0 && wrap && arc.path.closed(s)) {
              emit(s, a, arc.totals[s], false, true,
                   normalized ? &first_value : nullptr, nullptr, true,
                   normalized ? nullptr : &first_value);
              if (!Exact::zero(second_value)) {
                emit(s, im.integer(0), b, true, false, nullptr,
                     normalized ? &second_value : nullptr, false, nullptr,
                     normalized ? nullptr : &second_value);
              }
            } else {
              emit(s, a, b, start, finish, normalized ? &first_value : nullptr,
                   normalized ? &second_value : nullptr, true,
                   normalized ? nullptr : &first_value,
                   normalized ? nullptr : &second_value);
            }
            continue;
          }
          ranges.clear();
          std::size_t index = 0;
          auto remainder = phase;
          while (m.exact.order(remainder, pattern[index]) >= 0) {
            remainder = m.sub(remainder, pattern[index]);
            index = (index + 1) % pattern.size();
            work.poll();
          }
          auto position = m.exact.integer(0);
          bool finished = false;
          while (!finished) {
            work.poll();
            const auto next = m.add(position, m.sub(pattern[index], remainder));
            int order =
                arc.compare_target(s, arc.path.end(s), im.rational(next),
                                   arc.totals[s], nullptr, &next);
            if (order == 2) {
              throw IntegrationRetry{};
            }
            finished = order >= 0;
            if (!(index % 2) && m.exact.order(next, position) > 0) {
              if (!ranges.empty() && !ranges.back().end &&
                  m.exact.order(ranges.back().b, position) == 0) {
                ranges.back().b = next;
                ranges.back().end = finished;
              } else {
                operational_capacity(ranges.size() < maximum);
                ranges.push_back({position, next, finished});
              }
            }
            position = next;
            remainder = m.exact.integer(0);
            index = (index + 1) % pattern.size();
          }
          if (ranges.empty()) {
            continue;
          }
          if (ranges.size() == 1 && Exact::zero(ranges[0].a) && ranges[0].end) {
            copy_subpath(s);
            continue;
          }
          const bool seam = arc.path.closed(s) && ranges.size() > 1 &&
                            Exact::zero(ranges.front().a) && ranges.back().end;
          if (seam) {
            emit(s, im.rational(ranges.back().a), arc.totals[s], false, true,
                 nullptr, nullptr, true, &ranges.back().a);
            emit(s, im.integer(0), im.rational(ranges.front().b), true, false,
                 nullptr, nullptr, false, nullptr, &ranges.front().b);
          }
          for (std::size_t j = seam ? 1 : 0; j < ranges.size() - (seam ? 1 : 0);
               ++j) {
            const auto& range = ranges[j];
            emit(s, im.rational(range.a),
                 range.end ? arc.totals[s] : im.rational(range.b),
                 Exact::zero(range.a), range.end, nullptr, nullptr, true,
                 &range.a, range.end ? nullptr : &range.b);
          }
        }
        writer.finish();
        mapping.set(5, 0, mappings);
        mapping.set(5, 1, 1);
        write_id(mapping, 1, 0, source_id(reader, 0));
        // Width is a separate optional workflow value; absent width is not
        // reported as dropped merely because a provenance record is omitted.
        geometry_report(report, reader, 1);
        static_cast<void>(unmapped);
        path.publish(kernel, invocation, 0);
        mapping.publish(kernel, invocation, 1);
        report.publish(kernel, invocation, 2);
        return;
      } catch (const IntegrationRetry&) {
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "trim/dash true-arc certification unresolved",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation
namespace ps::plugin_internal {
Status register_path_trim(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    for (bool dash : {false, true}) {
      auto parameters = path_capacity_parameters();
      append(&parameters,
             {{"units", OperationParameterType::String, false},
              {"closed_wrap", OperationParameterType::Bool, false},
              {"restart", OperationParameterType::String, false},
              {"max_output_segments", OperationParameterType::Int64, false},
              {"max_integration_panels", OperationParameterType::Int64, false},
              {"max_refinements", OperationParameterType::Int64, false},
              {"max_scratch_bytes", OperationParameterType::Int64, false},
              {"limb_capacity", OperationParameterType::Int64, false}});
      auto op = operation(
          dash ? "path.dash_path_v1_strict" : "path.trim_path_v1_strict", 3,
          parameters,
          {{"path", take(ps::generation::core_path_schema({}))},
           {"source_map",
            take(ps::generation::path_source_map_schema(1, 1, 1))},
           {"report", take(ps::generation::geometry_report_schema(1))}},
          [dash](const auto& in, const auto& p) {
            return std::make_shared<TrimProgram>(in, p, dash);
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
