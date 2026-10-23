#include "03-generation/polygon_arrangement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ps::plugin_internal::generation {
namespace {
int sign(Math& m, const Rational& x) {
  m.work.poll();
  check(m.exact.status());
  return Exact::zero(x) ? 0 : x.negative ? -1 : 1;
}
bool selected(BooleanKind kind, bool a, bool b) {
  return kind == BooleanKind::Union          ? a || b
         : kind == BooleanKind::Intersection ? a && b
         : kind == BooleanKind::Difference   ? a && !b
                                             : a != b;
}
}  // namespace
BooleanKind boolean_kind(const Parameters& p) {
  const auto op = text(p, "operation", "union");
  require(op == "union" || op == "intersection" || op == "difference" ||
              op == "xor",
          "unknown polygon Boolean operation");
  return op == "union"          ? BooleanKind::Union
         : op == "intersection" ? BooleanKind::Intersection
         : op == "difference"   ? BooleanKind::Difference
                                : BooleanKind::Xor;
}
bool fill_evenodd(const Parameters& p, const char* key) {
  const auto rule = text(p, key, "nonzero");
  require(rule == "nonzero" || rule == "evenodd", "unknown polygon fill rule");
  return rule == "evenodd";
}
PolygonArrangement::PolygonArrangement(Math& math,
                                       std::pmr::memory_resource* memory,
                                       std::uint64_t events)
    : m(math),
      resource(memory),
      max_events(events),
      points(memory),
      sorted_points(memory),
      input_edges(memory),
      boundary(memory),
      vertices(memory),
      contours(memory) {
  // Geometry storage uses the invocation's bounded memory resource.
}
int PolygonArrangement::compare(const ExactPoint& a,
                                const ExactPoint& b) const {
  const int x = m.exact.order(a.x, b.x);
  const int result = x ? x : m.exact.order(a.y, b.y);
  check(m.exact.status());
  return result;
}
bool PolygonArrangement::equal(const ExactPoint& a, const ExactPoint& b) const {
  return compare(a, b) == 0;
}
ExactPoint PolygonArrangement::sub(const ExactPoint& a,
                                   const ExactPoint& b) const {
  return {m.sub(a.x, b.x), m.sub(a.y, b.y)};
}
ExactPoint PolygonArrangement::lerp(const ExactPoint& a, const ExactPoint& b,
                                    const Rational& t) const {
  return {m.lerp(a.x, b.x, t), m.lerp(a.y, b.y, t)};
}
Rational PolygonArrangement::cross(const ExactPoint& a,
                                   const ExactPoint& b) const {
  return m.sub(m.mul(a.x, b.y), m.mul(a.y, b.x));
}
Rational PolygonArrangement::dot(const ExactPoint& a,
                                 const ExactPoint& b) const {
  return m.add(m.mul(a.x, b.x), m.mul(a.y, b.y));
}
std::uint64_t PolygonArrangement::point(ExactPoint value) {
  auto where = std::lower_bound(
      sorted_points.begin(), sorted_points.end(), value,
      [&](auto id, const auto& p) { return compare(points[id], p) < 0; });
  if (where != sorted_points.end() && equal(points[*where], value)) {
    return *where;
  }
  operational_capacity(points.size() < max_events);
  const auto index = points.size();
  const auto offset = where - sorted_points.begin();
  points.push_back(std::move(value));
  sorted_points.insert(sorted_points.begin() + offset, index);
  return index;
}
void PolygonArrangement::edge(std::uint64_t a, std::uint64_t b,
                              unsigned source) {
  if (a == b) {
    return;
  }
  operational_capacity(input_edges.size() < max_events);
  input_edges.push_back({a, b, source});
}
Rational read_rational(const Reader& r, unsigned port, unsigned signs,
                       unsigned sizes, unsigned limbs, std::uint64_t row,
                       unsigned component, Math& m) {
  Rational value;
  value.d.words.fill(0);
  value.negative = r.bits(port, signs, {row, component}) != 0;
  for (unsigned side = 0; side < 2; ++side) {
    auto& words = side ? value.d.words : value.n.words;
    const auto count = static_cast<std::uint64_t>(
        r.index(port, sizes, {row, component, side}));
    operational_capacity(count <= 2 * Exact::kWords);
    for (std::uint64_t j = 0; j < count; ++j) {
      m.work.poll(4);
      std::uint64_t word = 0;
      for (unsigned byte = 0; byte < 4; ++byte) {
        word |= r.bits(port, limbs, {row, component, side, j, byte})
                << (8 * byte);
      }
      words[j / 2] |= word << (32 * (j % 2));
    }
  }
  return value;
}
void write_rational(const Dense& out, unsigned signs, unsigned sizes,
                    unsigned limbs, std::uint64_t row, unsigned component,
                    std::uint64_t capacity, const Rational& value) {
  out.set(signs, row * 2 + component, value.negative && !Exact::zero(value));
  for (unsigned side = 0; side < 2; ++side) {
    const auto& words = side ? value.d.words : value.n.words;
    std::uint64_t count = 2 * words.size();
    while (count > 1 && ((words[(count - 1) / 2] >> (32 * ((count - 1) % 2))) &
                         UINT64_C(0xffffffff)) == 0) {
      --count;
    }
    operational_capacity(count <= capacity);
    out.set(sizes, (row * 2 + component) * 2 + side, count);
    for (std::uint64_t j = 0; j < count; ++j) {
      out.work.poll(4);
      const auto word = words[j / 2] >> (32 * (j % 2));
      for (unsigned byte = 0; byte < 4; ++byte) {
        out.set(limbs,
                (((row * 2 + component) * 2 + side) * capacity + j) * 4 + byte,
                (word >> (8 * byte)) & 255);
      }
    }
  }
}
std::int64_t round_integer(Math& m, const Rational& value) {
  // All comparisons are rational, including half-grid ties at signed64 limits.
  const auto absolute = m.exact.absolute(value);
  std::uint64_t low = 0, high = UINT64_C(1) << 63;
  while (low < high) {
    m.work.poll();
    const auto mid = low + (high - low) / 2 + 1;
    Rational candidate;
    candidate.n.words[0] = mid;
    if (m.exact.order(candidate, absolute) <= 0) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  Rational floor;
  floor.n.words[0] = low;
  const int cmp = m.exact.order(m.sub(absolute, floor), m.exact.fraction(1, 2));
  if (cmp > 0 || (cmp == 0 && (low & 1))) {
    ++low;
  }
  check(m.exact.status());
  const auto raw = value.negative ? UINT64_C(0) - low : low;
  std::int64_t result;
  std::memcpy(&result, &raw, 8);
  return result;
}
double upward_sqrt(Math& m, const Rational& squared) {
  if (Exact::zero(squared)) {
    return 0;
  }
  auto result = number(m.rounded_sqrt(squared, false));
  if (m.exact.order(m.sq(m.value(result)), squared) < 0) {
    result = std::nextafter(result, std::numeric_limits<double>::infinity());
  }
  check(m.exact.status());
  return result;
}
void PolygonArrangement::read(const Reader& r, unsigned port, bool grid,
                              double spacing, double ox, double oy,
                              Rational* quantization_squared) {
  const auto& schema = *r.kernel.program->inputs[port].result_schema;
  const bool exact = schema.id == "photospider.ops.exact_polygon";
  const bool integers = schema.id == "photospider.ops.grid_polygon";
  const auto coordinate = [&](std::uint64_t row) {
    ExactPoint p;
    for (unsigned axis = 0; axis < 2; ++axis) {
      auto v = exact      ? read_rational(r, port, 0, 1, 2, row, axis, m)
               : integers ? m.exact.integer(r.index(port, 0, {row, axis}))
                          : m.input(r, port, 2, {row, axis});
      if (grid && !integers) {
        const auto rounded = round_integer(
            m, m.div(m.sub(v, m.value(axis ? oy : ox)), m.value(spacing)));
        const auto q = m.exact.integer(rounded);
        const auto error = m.sub(
            m.add(m.value(axis ? oy : ox), m.mul(m.value(spacing), q)), v);
        if (axis == 0) {
          p.x = m.sq(error);
        } else if (quantization_squared) {
          *quantization_squared =
              m.max(*quantization_squared, m.add(p.x, m.sq(error)));
        }
        v = q;
      }
      if (axis == 0) {
        p.y = v;
      } else {
        p.x = p.y;
        p.y = v;
      }
    }
    return point(std::move(p));
  };
  if (exact || integers) {
    const unsigned offsets = exact ? 3 : 1, counts = exact ? 4 : 2;
    const auto n = static_cast<std::uint64_t>(r.index(port, counts, {1}));
    for (std::uint64_t c = 0; c < n; ++c) {
      const auto begin =
          static_cast<std::uint64_t>(r.index(port, offsets, {c}));
      const auto end =
          static_cast<std::uint64_t>(r.index(port, offsets, {c + 1}));
      if (begin == end) {
        continue;
      }
      const auto first = coordinate(begin);
      auto previous = first;
      for (auto j = begin + 1; j < end; ++j) {
        const auto next = coordinate(j);
        edge(previous, next, port);
        previous = next;
      }
      edge(previous, first, port);
    }
    return;
  }
  const auto ns = static_cast<std::uint64_t>(r.index(port, 5, {2}));
  for (std::uint64_t s = 0; s < ns; ++s) {
    m.work.poll();
    const auto begin = static_cast<std::uint64_t>(r.index(port, 3, {s}));
    const auto end = static_cast<std::uint64_t>(r.index(port, 3, {s + 1}));
    const auto first = coordinate(r.index(port, 1, {begin}));
    auto previous = first;
    for (auto j = begin + 1; j < end; ++j) {
      if (r.bits(port, 0, {j}) == 5) {
        continue;
      }
      const auto next = coordinate(r.index(port, 1, {j}));
      edge(previous, next, port);
      previous = next;
    }
    edge(previous, first, port);
  }
}
bool PolygonArrangement::inside(const ExactPoint& p, unsigned source,
                                bool evenodd) const {
  std::int64_t winding = 0;
  for (const auto& e : input_edges) {
    if (e.source != source) {
      continue;
    }
    m.work.poll();
    const auto& a = points[e.a];
    const auto& b = points[e.b];
    const auto ay = m.exact.order(a.y, p.y), by = m.exact.order(b.y, p.y);
    const auto turn = sign(m, cross(sub(b, a), sub(p, a)));
    if (ay <= 0 && by > 0 && turn > 0) {
      ++winding;
    }
    if (by <= 0 && ay > 0 && turn < 0) {
      --winding;
    }
  }
  return evenodd ? (winding % 2) != 0 : winding != 0;
}
void PolygonArrangement::build(BooleanKind kind, bool evenodd_a,
                               bool evenodd_b) {
  struct Cut {
    std::uint64_t edge, vertex;
  };
  std::pmr::vector<Cut> cuts(resource);
  const auto add_cut = [&](std::uint64_t e, std::uint64_t v) {
    operational_capacity(cuts.size() < max_events * 4);
    cuts.push_back({e, v});
  };
  for (std::uint64_t i = 0; i < input_edges.size(); ++i) {
    add_cut(i, input_edges[i].a);
    add_cut(i, input_edges[i].b);
  }
  for (std::uint64_t i = 0; i < input_edges.size(); ++i) {
    const auto ai = input_edges[i].a, bi = input_edges[i].b;
    for (std::uint64_t j = i + 1; j < input_edges.size(); ++j) {
      m.work.poll();
      const auto aj = input_edges[j].a, bj = input_edges[j].b;
      const auto d = sub(points[bi], points[ai]),
                 e = sub(points[bj], points[aj]);
      const auto delta = sub(points[aj], points[ai]);
      const auto denominator = cross(d, e);
      if (!Exact::zero(denominator)) {
        const auto t = m.div(cross(delta, e), denominator);
        const auto u = m.div(cross(delta, d), denominator);
        if (sign(m, t) >= 0 && m.exact.order(t, m.exact.integer(1)) <= 0 &&
            sign(m, u) >= 0 && m.exact.order(u, m.exact.integer(1)) <= 0) {
          const auto intersection = point(lerp(points[ai], points[bi], t));
          add_cut(i, intersection);
          add_cut(j, intersection);
        }
      } else if (Exact::zero(cross(delta, d))) {
        const auto on = [&](std::uint64_t p, std::uint64_t a, std::uint64_t b) {
          return sign(m, dot(sub(points[p], points[a]),
                             sub(points[p], points[b]))) <= 0;
        };
        if (on(aj, ai, bi)) {
          add_cut(i, aj);
        }
        if (on(bj, ai, bi)) {
          add_cut(i, bj);
        }
        if (on(ai, aj, bj)) {
          add_cut(j, ai);
        }
        if (on(bi, aj, bj)) {
          add_cut(j, bi);
        }
      }
      check(m.exact.status());
    }
  }
  std::sort(cuts.begin(), cuts.end(), [&](const Cut& a, const Cut& b) {
    if (a.edge != b.edge) {
      return a.edge < b.edge;
    }
    const auto& e = input_edges[a.edge];
    const int order = compare(points[a.vertex], points[b.vertex]);
    return compare(points[e.a], points[e.b]) < 0 ? order < 0 : order > 0;
  });
  std::pmr::vector<ExactEdge> atoms(resource);
  for (std::size_t i = 1; i < cuts.size(); ++i) {
    if (cuts[i - 1].edge == cuts[i].edge &&
        cuts[i - 1].vertex != cuts[i].vertex) {
      auto a = cuts[i - 1].vertex, b = cuts[i].vertex;
      if (compare(points[a], points[b]) > 0) {
        std::swap(a, b);
      }
      atoms.push_back({a, b, 0});
    }
  }
  const auto edge_less = [](const ExactEdge& a, const ExactEdge& b) {
    return a.a != b.a ? a.a < b.a : a.b < b.b;
  };
  std::sort(atoms.begin(), atoms.end(), edge_less);
  atoms.erase(std::unique(atoms.begin(), atoms.end(),
                          [](const auto& a, const auto& b) {
                            return a.a == b.a && a.b == b.b;
                          }),
              atoms.end());
  for (const auto& atom : atoms) {
    m.work.poll();
    const auto midpoint =
        lerp(points[atom.a], points[atom.b], m.exact.fraction(1, 2));
    const auto delta = sub(points[atom.b], points[atom.a]);
    const ExactPoint normal{m.scale(delta.y, -1), delta.x};
    bool side[2]{};
    std::int64_t winding[2][2]{};
    const int normal_y = sign(m, normal.y);
    // Both infinitesimal sides share every constant predicate. Test the ray's
    // y range before constructing a cross product, then compute a linear
    // coefficient only when the exact constant term vanishes.
    for (const auto& edge : input_edges) {
      m.work.poll();
      const auto& a = points[edge.a];
      const auto& b = points[edge.b];
      const int constant_a = m.exact.order(a.y, midpoint.y);
      const int constant_b = m.exact.order(b.y, midpoint.y);
      bool upward[2], downward[2];
      for (unsigned j = 0; j < 2; ++j) {
        const int dy = j == 0 ? -normal_y : normal_y;
        const int ay = constant_a ? constant_a : dy;
        const int by = constant_b ? constant_b : dy;
        upward[j] = ay <= 0 && by > 0;
        downward[j] = by <= 0 && ay > 0;
      }
      if (!upward[0] && !upward[1] && !downward[0] && !downward[1]) {
        continue;
      }
      const auto direction = sub(b, a);
      const int constant_turn = sign(m, cross(direction, sub(midpoint, a)));
      const int linear_turn =
          constant_turn ? 0 : sign(m, cross(direction, normal));
      for (unsigned j = 0; j < 2; ++j) {
        const int turn = constant_turn ? constant_turn
                         : j == 0      ? linear_turn
                                       : -linear_turn;
        if (upward[j] && turn > 0) {
          ++winding[j][edge.source];
        }
        if (downward[j] && turn < 0) {
          --winding[j][edge.source];
        }
      }
    }
    check(m.exact.status());
    for (unsigned j = 0; j < 2; ++j) {
      const bool a = evenodd_a ? winding[j][0] % 2 != 0 : winding[j][0] != 0;
      const bool b = evenodd_b ? winding[j][1] % 2 != 0 : winding[j][1] != 0;
      side[j] = selected(kind, a, b);
    }
    if (side[0] != side[1]) {
      boundary.push_back(side[0] ? atom : ExactEdge{atom.b, atom.a, 0});
    }
  }
  canonicalize();
}
void PolygonArrangement::canonicalize() {
  std::pmr::vector<std::uint8_t> used(boundary.size(), 0, resource);
  std::pmr::vector<std::uint64_t> degrees(points.size(), 0, resource),
      cycle(resource);
  for (const auto& e : boundary) {
    ++degrees[e.a];
    ++degrees[e.b];
  }
  const auto angular_less = [&](const ExactPoint& a, const ExactPoint& b) {
    const auto half = [&](const ExactPoint& p) {
      const int y = sign(m, p.y);
      return y < 0 || (y == 0 && sign(m, p.x) < 0);
    };
    const bool ha = half(a), hb = half(b);
    return ha != hb ? ha < hb : sign(m, cross(a, b)) > 0;
  };
  for (std::uint64_t start = 0; start < boundary.size(); ++start) {
    if (used[start]) {
      continue;
    }
    cycle.clear();
    auto current = start;
    do {
      m.work.poll();
      operational_capacity(cycle.size() <= boundary.size() && !used[current]);
      used[current] = 1;
      const auto edge = boundary[current];
      cycle.push_back(edge.a);
      const auto reverse = sub(points[edge.a], points[edge.b]);
      std::uint64_t next = UINT64_MAX, wrap = UINT64_MAX;
      for (std::uint64_t j = 0; j < boundary.size(); ++j) {
        if (boundary[j].a != edge.b) {
          continue;
        }
        const auto direction = sub(points[boundary[j].b], points[edge.b]);
        if (wrap == UINT64_MAX ||
            angular_less(sub(points[boundary[wrap].b], points[edge.b]),
                         direction)) {
          wrap = j;
        }
        if (angular_less(direction, reverse) &&
            (next == UINT64_MAX ||
             angular_less(sub(points[boundary[next].b], points[edge.b]),
                          direction))) {
          next = j;
        }
      }
      current = next == UINT64_MAX ? wrap : next;
      if (current == UINT64_MAX) {
        invalid_quality("polygon arrangement has no successor");
      }
    } while (current != start);
    bool changed = true;
    while (changed && cycle.size() > 2) {
      changed = false;
      for (std::size_t j = 0; j < cycle.size(); ++j) {
        const auto before = cycle[(j + cycle.size() - 1) % cycle.size()];
        const auto middle = cycle[j], after = cycle[(j + 1) % cycle.size()];
        if (degrees[middle] == 2 &&
            Exact::zero(cross(sub(points[middle], points[before]),
                              sub(points[after], points[middle])))) {
          cycle.erase(cycle.begin() + j);
          changed = true;
          break;
        }
      }
    }
    if (cycle.size() < 3) {
      continue;
    }
    const auto rotation_less = [&](std::size_t a, std::size_t b) {
      for (std::size_t j = 0; j < cycle.size(); ++j) {
        const int cmp = compare(points[cycle[(a + j) % cycle.size()]],
                                points[cycle[(b + j) % cycle.size()]]);
        if (cmp) {
          return cmp < 0;
        }
      }
      return false;
    };
    std::size_t first = 0;
    for (std::size_t j = 1; j < cycle.size(); ++j) {
      if (rotation_less(j, first)) {
        first = j;
      }
    }
    const auto begin = vertices.size();
    for (std::size_t j = 0; j < cycle.size(); ++j) {
      vertices.push_back(cycle[(first + j) % cycle.size()]);
    }
    contours.push_back({begin, vertices.size()});
  }
  std::sort(contours.begin(), contours.end(),
            [&](const auto& a, const auto& b) {
              for (std::uint64_t i = 0;
                   i < std::min(a.end - a.begin, b.end - b.begin); ++i) {
                const int cmp = compare(points[vertices[a.begin + i]],
                                        points[vertices[b.begin + i]]);
                if (cmp) {
                  return cmp < 0;
                }
              }
              return a.end - a.begin < b.end - b.begin;
            });
}
Rational PolygonArrangement::squared_distance(const ExactPoint& query,
                                              ExactPoint* closest) const {
  bool found = false;
  Rational best;
  for (const auto& e : boundary) {
    m.work.poll();
    const auto d = sub(points[e.b], points[e.a]);
    const auto denominator = dot(d, d);
    const auto t =
        m.min(m.exact.integer(1),
              m.max(m.exact.integer(0),
                    m.div(dot(sub(query, points[e.a]), d), denominator)));
    const auto p = lerp(points[e.a], points[e.b], t);
    const auto delta = sub(query, p);
    const auto distance = dot(delta, delta);
    if (!found || m.exact.order(distance, best) < 0) {
      best = distance;
      if (closest) {
        *closest = p;
      }
      found = true;
    }
  }
  if (!found) {
    no_solution("empty polygon boundary");
  }
  return best;
}
}  // namespace ps::plugin_internal::generation
