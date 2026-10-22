#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "03-generation/paths.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct Edge final {
  double x0, y0, x1, y1;
  int winding;
};
struct Event final {
  unsigned kind;
  std::uint64_t a, b;
};
struct Polygon final : Program, Canvas {
  std::uint64_t edge_capacity, event_capacity, edge_offset, event_offset,
      index_offset;
  bool evenodd;
  Polygon(const std::vector<OperationMetadata>& inputs, const Parameters& p)
      : Canvas(p) {
    const auto info = path_info(*inputs[0].result_schema);
    require(!info.primitive, "polygon area requires Core M/L/Z paths");
    edge_capacity = info.capacities[0];
    const auto rule = text(p, "fill_rule", "nonzero");
    require(rule == "nonzero" || rule == "evenodd", "polygon fill rule");
    evenodd = rule == "evenodd";
    const auto requested =
        static_cast<std::uint64_t>(integer(p, "max_events", 65536));
    require(requested >= 2 && requested <= (UINT64_C(1) << 32),
            "polygon event capacity");
    const auto bound = static_cast<unsigned __int128>(edge_capacity) *
                           (edge_capacity - 1) / 2 +
                       4 * edge_capacity + 2;
    event_capacity = static_cast<std::uint64_t>(
        std::min(bound, static_cast<unsigned __int128>(requested)));
    edge_offset = sizeof(Math);
    event_offset = edge_offset + edge_capacity * sizeof(Edge);
    index_offset = event_offset + event_capacity * sizeof(Event);
    scratch_bytes = index_offset + 2 * edge_capacity * sizeof(std::uint64_t);
    add_output(this, tensor_schema(type, {height, width}, true));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* base = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arena(base, work);
    auto& m = *arena;
    auto& e = m.exact;
    auto* edges = reinterpret_cast<Edge*>(base + edge_offset);
    auto* events = reinterpret_cast<Event*>(base + event_offset);
    auto* candidates = reinterpret_cast<std::uint64_t*>(base + index_offset);
    auto* active = candidates + edge_capacity;
    std::uint64_t edge_count = 0;
    const auto point = [&](std::uint64_t control) {
      return std::array<double, 2>{reader.number(0, 2, {control, 0}),
                                   reader.number(0, 2, {control, 1})};
    };
    const auto add_edge = [&](const auto& a, const auto& b) {
      work.poll();
      if (a[1] == b[1]) {
        return;
      }
      operational_capacity(edge_count < edge_capacity);
      edges[edge_count++] = {a[0], a[1], b[0], b[1], a[1] < b[1] ? 1 : -1};
    };
    const auto count = path_counts(reader, 0, false);
    for (std::uint64_t s = 0; s < count[2]; ++s) {
      work.poll();
      const auto begin = static_cast<std::uint64_t>(reader.index(0, 3, {s}));
      const auto end = static_cast<std::uint64_t>(reader.index(0, 3, {s + 1}));
      const auto anchor = point(reader.index(0, 1, {begin}));
      auto previous = anchor;
      bool closed = false;
      for (auto row = begin + 1; row < end; ++row) {
        work.poll();
        if (reader.bits(0, 0, {row}) == 5) {
          add_edge(previous, anchor);
          closed = true;
        } else {
          const auto next = point(reader.index(0, 1, {row}));
          add_edge(previous, next);
          previous = next;
        }
      }
      if (!closed) {
        add_edge(previous, anchor);
      }
    }
    const auto x_at = [&](std::uint64_t index, const Rational& y) {
      const auto& edge = edges[index];
      return m.add(m.value(edge.x0),
                   m.mul(m.div(m.sub(y, m.value(edge.y0)),
                               m.sub(m.value(edge.y1), m.value(edge.y0))),
                         m.sub(m.value(edge.x1), m.value(edge.x0))));
    };
    samples(invocation.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      e.reset();
      const auto x0 = m.add(e.integer(origin_x), e.integer(at[1])),
                 x1 = m.add(x0, e.integer(1));
      const auto y0 = m.add(e.integer(origin_y), e.integer(at[0])),
                 y1 = m.add(y0, e.integer(1));
      std::uint64_t nc = 0, ne = 0;
      for (std::uint64_t i = 0; i < edge_count; ++i) {
        work.poll();
        if (e.order(m.value(std::max(edges[i].y0, edges[i].y1)), y0) > 0 &&
            e.order(m.value(std::min(edges[i].y0, edges[i].y1)), y1) < 0) {
          candidates[nc++] = i;
        }
      }
      const auto slope = [&](std::uint64_t i) {
        return m.div(m.sub(m.value(edges[i].x1), m.value(edges[i].x0)),
                     m.sub(m.value(edges[i].y1), m.value(edges[i].y0)));
      };
      const auto event_y = [&](const Event& event) -> Rational {
        if (event.kind == 0) {
          return event.a ? y1 : y0;
        }
        const auto& a = edges[event.a];
        if (event.kind == 1) {
          return m.value(event.b ? a.y1 : a.y0);
        }
        if (event.kind == 2) {
          return m.add(
              m.value(a.y0),
              m.div(m.sub(event.b ? x1 : x0, m.value(a.x0)), slope(event.a)));
        }
        const auto sa = slope(event.a), sb = slope(event.b);
        const auto ba = m.sub(m.value(a.x0), m.mul(sa, m.value(a.y0)));
        const auto& b = edges[event.b];
        const auto bb = m.sub(m.value(b.x0), m.mul(sb, m.value(b.y0)));
        return m.div(m.sub(bb, ba), m.sub(sa, sb));
      };
      const auto insert = [&](Event event, const Rational& y) {
        if (e.order(y, y0) <= 0 || e.order(y, y1) >= 0) {
          return;
        }
        if (event.kind >= 2) {
          const auto inside = [&](std::uint64_t i) {
            return e.order(y, m.value(std::min(edges[i].y0, edges[i].y1))) >=
                       0 &&
                   e.order(y, m.value(std::max(edges[i].y0, edges[i].y1))) <= 0;
          };
          if (!inside(event.a) || (event.kind == 3 && !inside(event.b))) {
            return;
          }
        }
        operational_capacity(ne < event_capacity);
        events[ne++] = event;
      };
      events[ne++] = {0, 0, 0};
      events[ne++] = {0, 1, 0};
      for (std::uint64_t a = 0; a < nc; ++a) {
        work.poll();
        const auto i = candidates[a];
        for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
          const Event vertex{1, i, endpoint};
          insert(vertex, event_y(vertex));
          if (edges[i].x0 != edges[i].x1) {
            const Event crossing{2, i, endpoint};
            insert(crossing, event_y(crossing));
          }
        }
        for (std::uint64_t b = 0; b < a; ++b) {
          work.poll();
          const auto j = candidates[b];
          if (e.order(slope(i), slope(j)) == 0) {
            continue;
          }
          const Event intersection{3, i, j};
          insert(intersection, event_y(intersection));
        }
      }
      std::sort(events, events + ne, [&](const Event& a, const Event& b) {
        work.poll();
        const auto order = e.order(event_y(a), event_y(b));
        check(e.status());
        return order < 0;
      });
      auto area = e.integer(0), lower = y0;
      for (std::uint64_t cut = 1; cut < ne; ++cut) {
        work.poll();
        const auto upper = event_y(events[cut]);
        if (e.order(upper, lower) == 0) {
          continue;
        }
        const auto middle = m.div(m.add(lower, upper), e.integer(2));
        std::uint64_t na = 0;
        for (std::uint64_t j = 0; j < nc; ++j) {
          const auto i = candidates[j];
          if (e.order(middle, m.value(std::min(edges[i].y0, edges[i].y1))) >
                  0 &&
              e.order(middle, m.value(std::max(edges[i].y0, edges[i].y1))) <
                  0) {
            active[na++] = i;
          }
        }
        std::sort(active, active + na, [&](auto a, auto b) {
          work.poll();
          const auto order = e.order(x_at(a, middle), x_at(b, middle));
          check(e.status());
          return order < 0;
        });
        std::int64_t winding = 0;
        std::uint64_t j = 0;
        while (j < na) {
          const auto left = active[j];
          const auto location = x_at(left, middle);
          do {
            winding += edges[active[j]].winding;
            ++j;
            work.poll();
          } while (j < na && e.order(x_at(active[j], middle), location) == 0);
          if (j == na || (evenodd ? (winding % 2 == 0) : winding == 0)) {
            continue;
          }
          const auto right = active[j];
          const auto span = [&](const Rational& y) {
            return m.positive(
                m.sub(m.min(x1, x_at(right, y)), m.max(x0, x_at(left, y))));
          };
          area = m.add(area, m.div(m.mul(m.add(span(lower), span(upper)),
                                         m.sub(upper, lower)),
                                   e.integer(2)));
        }
        lower = upper;
      }
      store(invocation.outputs[0], at,
            m.rounded(area, type == ElementType::Float32));
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation
namespace ps::plugin_internal {
Status register_polygon_area(OperationRegistry* registry) {
  using generation::canvas_parameters;
  using generation::check;
  using generation::operation;
  using generation::Polygon;
  using generation::tensor_schema;
  try {
    auto parameters = canvas_parameters();
    parameters.push_back({"fill_rule", OperationParameterType::String, false});
    parameters.push_back({"max_events", OperationParameterType::Int64, false});
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned profile = 0; profile < 3; ++profile) {
      check(registry->register_operation(operation(
          std::string("path.fill_polygon_area_v1_") + suffix[profile], 1,
          parameters,
          {{"coverage", tensor_schema(ElementType::Float64, {1, 1}, true)}},
          [profile](const auto& inputs, const auto& p) {
            check(numeric_ops::sequence_profile_available(
                static_cast<numeric_ops::SequenceProfile>(profile)));
            return std::make_shared<Polygon>(inputs, p);
          })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
