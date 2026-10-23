#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "03-generation/path_arc.hpp"
#include "03-generation/polygon_arrangement.hpp"

namespace ps::plugin_internal::generation {
// Materialize the explicitly bound source profile at target polyline vertices.
// RN64 widths are an observable stage before the variable-round disk sweep.
struct BoundWidthValues final : Program {
  PathInfo target;
  std::vector<PathInfo> sources;
  bool mapped;
  unsigned source_begin;
  std::uint64_t cache_count = 0, panels, refinements;
  DenseLayout layout{SchemaTemplate{}};
  BoundWidthValues(const std::vector<OperationMetadata>& in,
                   const Parameters& params, bool has_map)
      : target(path_info(*in[0].result_schema)),
        mapped(has_map),
        source_begin(has_map ? 3 : 2),
        panels(integer(params, "max_integration_panels", 1048576)),
        refinements(integer(params, "max_refinements", 4096)) {
    require(!target.primitive, "bound widths require a CoreVerbs polyline");
    const auto extent = [](const SchemaTemplate& s, unsigned member,
                           unsigned axis, unsigned rank) {
      const auto shape = s.tensors[member].sample_shape();
      require(shape.size() == rank, "associated Result member rank mismatch");
      return shape[axis];
    };
    const auto& w = *in[1].result_schema;
    require(w.id == "photospider.ops.path_width" && w.version == 1 &&
                w.tensors.size() == 6,
            "expected Linear path_width v1");
    const auto expected = take(ps::generation::path_width_schema(
        extent(w, 0, 0, 2), extent(w, 1, 0, 2), extent(w, 2, 0, 1),
        extent(w, 4, 0, 2)));
    require(expected.same_schema(w), "width binding directory mismatch");
    if (mapped) {
      const auto& map = *in[2].result_schema;
      require(map.id == "photospider.ops.path_source_map" && map.version == 1 &&
                  map.tensors.size() == 6,
              "expected path_source_map v1");
      const auto expected_map = take(ps::generation::path_source_map_schema(
          extent(map, 0, 0, 2), extent(map, 1, 0, 2), extent(map, 4, 3, 5)));
      require(expected_map.same_schema(map), "source map directory mismatch");
    }
    require(panels && refinements, "invalid width integration budgets");
    for (unsigned port = source_begin; port < in.size(); ++port) {
      sources.push_back(path_info(*in[port].result_schema));
      cache_count = std::max(
          cache_count,
          checked_add(
              size_product({2, checked_add(sources.back().capacities[0], 1)}),
              subpath_capacity(sources.back())));
    }
    add_output(this,
               tensor_schema(ElementType::Float64, {target.capacities[1]}));
    layout = DenseLayout(*outputs[0].result_schema);
    scratch_bytes = checked_add(
        sizeof(CurveIntegralScratch),
        checked_add(size_product({cache_count, sizeof(OwnedInterval)}),
                    layout.bytes));
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
    auto* cache =
        reinterpret_cast<OwnedInterval*>(base + sizeof(CurveIntegralScratch));
    for (std::uint64_t j = 0; j < cache_count; ++j) {
      new (cache + j) OwnedInterval{};
    }
    const Dense out{layout,
                    base + sizeof(CurveIntegralScratch) +
                        cache_count * sizeof(OwnedInterval),
                    work};
    out.clear();
    auto& m = storage->evaluation.math;
    m.max_refinements = refinements;
    PathCursor destination(reader, target, m);
    const auto binding_count =
        static_cast<std::uint64_t>(reader.index(1, 5, {0}));
    const auto mapping_count =
        mapped ? static_cast<std::uint64_t>(reader.index(2, 5, {0})) : 0;
    for (unsigned slot = 0; slot < sources.size(); ++slot) {
      const Reader source{kernel, invocation, source_begin + slot};
      CurveIntegral integral(source, *storage, panels);
      auto& im = integral.im;
      const auto& info = sources[slot];
      ArcTraversal arc(integral, info, cache, cache + info.capacities[0] + 1,
                       cache + 2 * (info.capacities[0] + 1));
      bool done = false;
      for (unsigned precision = 128, attempt = 0;
           precision <= 4096 && attempt < refinements;
           precision *= 2, ++attempt) {
        m.d.reset();
        m.d.precision = precision;
        try {
          arc.index();
          auto total = im.integer(0);
          for (std::uint64_t s = 0; s < arc.path.subpaths(); ++s) {
            total = im.add(total, arc.totals[s]);
          }
          for (std::uint64_t subpath = 0; subpath < destination.subpaths();
               ++subpath) {
            const auto first = destination.first(subpath),
                       end = destination.end(subpath);
            const auto start_control =
                static_cast<std::uint64_t>(reader.index(0, 1, {first - 1}));
            const auto positive = [&](std::uint64_t row) {
              const auto a =
                  static_cast<std::uint64_t>(reader.index(0, 1, {row})) - 1;
              const auto b =
                  reader.bits(0, 0, {row}) == 5 ? start_control : a + 1;
              return reader.number(0, 2, {a, 0}) !=
                         reader.number(0, 2, {b, 0}) ||
                     reader.number(0, 2, {a, 1}) != reader.number(0, 2, {b, 1});
            };
            for (auto vertex_row = first - 1; vertex_row < end; ++vertex_row) {
              if (reader.bits(0, 0, {vertex_row}) == 5) {
                continue;
              }
              const auto control =
                  static_cast<std::uint64_t>(reader.index(0, 1, {vertex_row}));
              auto owner = vertex_row + 1;
              while (owner < end && !positive(owner)) {
                work.poll();
                ++owner;
              }
              unsigned endpoint = 0;
              if (owner == end) {
                while (owner > first && !positive(owner - 1)) {
                  work.poll();
                  --owner;
                }
                if (owner > first) {
                  --owner;
                  endpoint = 1;
                } else {
                  owner = end;
                }
              }
              std::uint64_t source_subpath = subpath, source_row = owner;
              auto t = m.exact.integer(endpoint);
              if (mapped && owner < end) {
                std::uint64_t record = 0;
                while (record < mapping_count &&
                       reader.index(2, 0, {record, 0}) !=
                           static_cast<std::int64_t>(owner)) {
                  work.poll();
                  ++record;
                }
                if (record == mapping_count) {
                  invalid_quality(
                      "a required vertex source map is not representable");
                }
                source_subpath = reader.index(2, 0, {record, 2});
                source_row = reader.index(2, 0, {record, 3});
                t = m.add(
                    read_rational(reader, 2, 2, 3, 4, record, 0, m),
                    m.scale(read_rational(reader, 2, 2, 3, 4, record, 1, m),
                            endpoint));
              }
              std::uint64_t binding = 0;
              while (binding < binding_count &&
                     reader.index(1, 1, {binding, 0}) !=
                         static_cast<std::int64_t>(mapped ? source_subpath
                                                          : subpath)) {
                work.poll();
                ++binding;
              }
              const auto profile =
                  static_cast<std::uint64_t>(reader.index(1, 1, {binding, 1}));
              if (reader.index(1, 0, {profile, 0}) !=
                  static_cast<std::int64_t>(slot)) {
                continue;
              }
              const auto domain = reader.index(1, 0, {profile, 1});
              if (domain != 0) {
                source_subpath = reader.index(1, 1, {binding, 2});
              }
              auto s = im.integer(0);
              if (owner < end) {
                s = im.add(arc.prefixes[source_row],
                           arc.partial(source_row, t));
              }
              const auto length =
                  domain == 0 ? total : arc.totals[source_subpath];
              if (domain == 0) {
                for (std::uint64_t previous = 0; previous < source_subpath;
                     ++previous) {
                  s = im.add(s, arc.totals[previous]);
                }
              }
              auto u = domain == 2        ? s
                       : arc.zero(length) ? im.integer(0)
                                          : im.div(s, length);
              if (domain != 2 && owner < end &&
                  (domain != 0 || arc.path.subpaths() == 1) &&
                  arc.path.end(source_subpath) -
                          arc.path.first(source_subpath) ==
                      1) {
                const auto curve = arc.path.curve(source_row);
                integral.initialize(curve);
                if ((curve.kind == CurveKind::Bezier && curve.degree <= 1) ||
                    curve.kind == CurveKind::Close || integral.circle) {
                  u = im.rational(t);
                }
              }
              const auto begin_stop =
                  static_cast<std::uint64_t>(reader.index(1, 0, {profile, 2}));
              const auto end_stop =
                  static_cast<std::uint64_t>(reader.index(1, 0, {profile, 3}));
              std::uint64_t answer = 0;
              if (arc.point(u)) {
                const auto query = im.exact_number(u.low);
                auto k = begin_stop;
                while (k + 1 < end_stop &&
                       m.exact.order(m.input(reader, 1, 2, {k}), query) < 0) {
                  work.poll();
                  ++k;
                }
                const auto x = m.input(reader, 1, 2, {k});
                auto value = m.input(reader, 1, 3, {k});
                if (m.exact.order(x, query) != 0) {
                  const auto a = m.input(reader, 1, 2, {k - 1});
                  value = m.lerp(m.input(reader, 1, 3, {k - 1}), value,
                                 m.div(m.sub(query, a), m.sub(x, a)));
                }
                answer = m.rounded(value, false, true);
              } else {
                OwnedInterval bound;
                bool found = false;
                for (auto k = begin_stop; k + 1 < end_stop; ++k) {
                  work.poll();
                  const auto a = im.rational(m.input(reader, 1, 2, {k}));
                  const auto b = im.rational(m.input(reader, 1, 2, {k + 1}));
                  if (m.d.compare(u.high, a.low) < 0 ||
                      m.d.compare(u.low, b.high) > 0) {
                    continue;
                  }
                  const auto local =
                      im.div(im.sub(im.intersect(u, {a.low, b.high}), a),
                             im.sub(b, a));
                  const auto wa = im.rational(m.input(reader, 1, 3, {k}));
                  const auto value = im.add(
                      wa,
                      im.mul(local,
                             im.sub(im.rational(m.input(reader, 1, 3, {k + 1})),
                                    wa)));
                  bound = found ? im.hull(bound, value) : value;
                  found = true;
                }
                if (!found || !im.rounded(bound, false, &answer)) {
                  throw IntegrationRetry{};
                }
              }
              out.set(0, control, answer);
            }
          }
          im.check_state();
          done = true;
          break;
        } catch (const IntegrationRetry&) {
        }
      }
      if (!done) {
        throw Status{ErrorCode::OperationFailed,
                     "source width certification unresolved",
                     DomainErrorCode{UINT64_C(0x47454e0001)}};
      }
    }
    out.publish(kernel, invocation, 0);
  }
};
}  // namespace ps::plugin_internal::generation
