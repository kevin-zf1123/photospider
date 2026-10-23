#include "03-generation/circular_arrangement.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ps::plugin_internal::generation {
CircularArrangement::CircularArrangement(Radical& arithmetic,
                                         std::pmr::memory_resource* memory,
                                         std::uint64_t max_events)
    : r(arithmetic),
      p(r),
      resource(memory),
      points(memory),
      polygon_points(memory),
      point_order(memory),
      lines(memory),
      original(memory),
      circles(memory),
      shapes(memory),
      cuts(memory),
      boundary(memory),
      cycles(memory),
      cycle_offsets(memory),
      maximum(max_events) {
  // Geometry storage uses the invocation's bounded memory resource.
}
void CircularArrangement::event(std::uint64_t count) {
  r.m.work.poll(count);
  if (count > maximum - std::min(maximum, events)) {
    throw Status{ErrorCode::ResourceExhausted,
                 "circular arrangement event capacity",
                 FailureReason::CapacityLimit};
  }
  events += count;
}
std::uint32_t CircularArrangement::vertex(Point value) {
  auto at = std::lower_bound(
      point_order.begin(), point_order.end(), value,
      [&](auto index, Point q) { return p.compare(points[index], q) < 0; });
  if (at != point_order.end() && !p.compare(points[*at], value)) {
    return *at;
  }
  event();
  const auto id = static_cast<std::uint32_t>(points.size());
  points.push_back(value);
  point_order.insert(at, id);
  return id;
}
void CircularArrangement::line(Point a, Point b) {
  if (!p.compare(a, b)) {
    return;
  }
  const auto x = vertex(a), y = vertex(b);
  for (const auto& old : lines) {
    if ((old.a == x && old.b == y) || (old.a == y && old.b == x)) {
      return;
    }
  }
  event();
  lines.push_back({x, y});
}
void CircularArrangement::circle(Point center, Id radius) {
  if (!r.sign(radius)) {
    return;
  }
  for (const auto& old : circles) {
    if (!p.compare(center, old.center) && !r.compare(radius, old.radius)) {
      return;
    }
  }
  event();
  circles.push_back({center, radius});
}
void CircularArrangement::polygon(const Point* values, std::size_t count) {
  if (count < 3) {
    return;
  }
  Id area = 0;
  for (std::size_t j = 0; j < count; ++j) {
    area = r.add(area, p.cross(values[j], values[(j + 1) % count]));
  }
  const auto orientation = r.sign(area);
  if (!orientation) {
    return;
  }
  Shape shape;
  shape.begin = static_cast<std::uint32_t>(polygon_points.size());
  shape.count = static_cast<std::uint32_t>(count);
  for (std::size_t j = 0; j < count; ++j) {
    polygon_points.push_back(values[orientation > 0 ? j : count - 1 - j]);
  }
  for (std::size_t j = 0; j < count; ++j) {
    line(polygon_points[shape.begin + j],
         polygon_points[shape.begin + (j + 1) % count]);
  }
  shapes.push_back(shape);
}
void CircularArrangement::disk(Point center, Id radius) {
  if (!r.sign(radius)) {
    return;
  }
  Shape shape;
  shape.kind = Shape::Disk;
  shape.a = center;
  shape.radius_a = radius;
  shapes.push_back(shape);
  circle(center, radius);
}
void CircularArrangement::sector(Point center, Id radius, Point start,
                                 Point end) {
  if (!r.sign(radius)) {
    return;
  }
  Shape shape;
  shape.kind = Shape::Sector;
  shape.a = center;
  shape.radius_a = radius;
  shape.begin = static_cast<std::uint32_t>(polygon_points.size());
  polygon_points.push_back(start);
  polygon_points.push_back(end);
  shapes.push_back(shape);
  circle(center, radius);
  line(center,
       p.add(center, p.mul(start, r.div(radius, r.sqrt(p.norm(start))))));
  line(center, p.add(center, p.mul(end, r.div(radius, r.sqrt(p.norm(end))))));
}
void CircularArrangement::hull(Point a, Point b, Id ra, Id rb) {
  const auto v = p.sub(b, a);
  const auto squared = p.norm(v), delta = r.sub(rb, ra),
             k = r.sub(squared, r.sq(delta));
  if (r.sign(k) <= 0) {
    disk(r.compare(ra, rb) >= 0 ? a : b, r.max(ra, rb));
    return;
  }
  if (!r.sign(ra) && !r.sign(rb)) {
    return;
  }
  Shape shape;
  shape.kind = Shape::Hull;
  shape.a = a;
  shape.b = b;
  shape.radius_a = ra;
  shape.radius_b = rb;
  shape.squared = squared;
  shape.delta = delta;
  shape.k = k;
  shapes.push_back(shape);
  circle(a, ra);
  circle(b, rb);
  const auto root = r.sqrt(k);
  for (int side : {-1, 1}) {
    const auto normal =
        p.mul(p.add(p.mul(v, r.neg(delta)),
                    p.mul(p.perp(v), r.mul(r.integer(side), root))),
              r.inverse(squared));
    line(p.add(a, p.mul(normal, ra)), p.add(b, p.mul(normal, rb)));
  }
}
void CircularArrangement::offset_edge(Point a, Point b, Id radius) {
  const auto ia = vertex(a), ib = vertex(b);
  if (ia == ib) {
    return;
  }
  original.push_back({ia, ib});
  if (!r.sign(radius)) {
    line(a, b);
    return;
  }
  const auto direction = p.sub(b, a);
  const auto normal =
      p.mul(p.perp(direction), r.div(radius, r.sqrt(p.norm(direction))));
  line(p.add(a, normal), p.add(b, normal));
  line(p.sub(a, normal), p.sub(b, normal));
  circle(a, radius);
  circle(b, radius);
}
CircularArrangement::Quadratic CircularArrangement::add(Quadratic a,
                                                        Quadratic b) {
  for (unsigned j = 0; j < 3; ++j) {
    a.c[j] = r.add(a.c[j], b.c[j]);
  }
  return a;
}
CircularArrangement::Quadratic CircularArrangement::sub(Quadratic a,
                                                        Quadratic b) {
  for (unsigned j = 0; j < 3; ++j) {
    a.c[j] = r.sub(a.c[j], b.c[j]);
  }
  return a;
}
CircularArrangement::Quadratic CircularArrangement::mul(Quadratic a,
                                                        Quadratic b) {
  Quadratic out;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j + i < 3; ++j) {
      out.c[i + j] = r.add(out.c[i + j], r.mul(a.c[i], b.c[j]));
    }
  }
  return out;
}
CircularArrangement::Quadratic CircularArrangement::coordinate(Probe q,
                                                               bool y) {
  return {{y ? q.point.y : q.point.x, y ? q.direction.y : q.direction.x, 0}};
}
CircularArrangement::Quadratic CircularArrangement::dot_probe(Point v,
                                                              Probe q) {
  return {{p.dot(v, q.point), p.dot(v, q.direction), 0}};
}
CircularArrangement::Quadratic CircularArrangement::norm_probe(Probe q) {
  return {{p.norm(q.point), r.mul(r.integer(2), p.dot(q.point, q.direction)),
           p.norm(q.direction)}};
}
int CircularArrangement::sign(Quadratic value) {
  for (auto c : value.c) {
    const int side = r.sign(c);
    if (side) {
      return side;
    }
  }
  return 0;
}
bool CircularArrangement::inside_shape(const Shape& shape, Probe q) {
  if (shape.kind == Shape::Polygon) {
    for (std::uint32_t j = 0; j < shape.count; ++j) {
      const auto a = polygon_points[shape.begin + j];
      const auto b = polygon_points[shape.begin + (j + 1) % shape.count];
      const auto v = p.sub(b, a);
      if (sign({{p.cross(v, p.sub(q.point, a)), p.cross(v, q.direction), 0}}) <
          0) {
        return false;
      }
    }
    return true;
  }
  const Probe relative{p.sub(q.point, shape.a), q.direction};
  const auto first = sub(norm_probe(relative), constant(r.sq(shape.radius_a)));
  if (shape.kind == Shape::Disk) {
    return sign(first) <= 0;
  }
  if (shape.kind == Shape::Sector) {
    const auto a = polygon_points[shape.begin],
               b = polygon_points[shape.begin + 1];
    return sign(first) <= 0 &&
           sign({{p.cross(a, relative.point), p.cross(a, q.direction), 0}}) >=
               0 &&
           sign({{p.cross(relative.point, b), p.cross(q.direction, b), 0}}) >=
               0;
  }
  const auto v = p.sub(shape.b, shape.a);
  const auto projection =
      add(dot_probe(v, relative), constant(r.mul(shape.radius_a, shape.delta)));
  if (sign(projection) <= 0) {
    return sign(first) <= 0;
  }
  if (sign(sub(projection, constant(shape.k))) >= 0) {
    return sign(sub(norm_probe({p.sub(q.point, shape.b), q.direction}),
                    constant(r.sq(shape.radius_b)))) <= 0;
  }
  return sign(sub(mul(constant(shape.k), first),
                  mul(projection, projection))) <= 0;
}
bool CircularArrangement::inside_original(Probe q) {
  std::int64_t winding = 0;
  for (const auto& edge : original) {
    const auto a = points[edge.a], b = points[edge.b], v = p.sub(b, a);
    const auto ay = sign({{r.sub(a.y, q.point.y), r.neg(q.direction.y), 0}});
    const auto by = sign({{r.sub(b.y, q.point.y), r.neg(q.direction.y), 0}});
    const auto side =
        sign({{p.cross(v, p.sub(q.point, a)), p.cross(v, q.direction), 0}});
    if (ay <= 0 && by > 0 && side > 0) {
      ++winding;
    }
    if (by <= 0 && ay > 0 && side < 0) {
      --winding;
    }
  }
  return winding != 0;
}
bool CircularArrangement::near_original(Probe q) {
  for (const auto& edge : original) {
    const auto a = points[edge.a], b = points[edge.b], v = p.sub(b, a);
    const Probe relative{p.sub(q.point, a), q.direction};
    const auto projection = dot_probe(v, relative);
    const auto squared = p.norm(v);
    Quadratic comparison;
    if (sign(projection) <= 0) {
      comparison = sub(norm_probe(relative), constant(r.sq(offset_radius)));
    } else if (sign(sub(projection, constant(squared))) >= 0) {
      comparison = sub(norm_probe({p.sub(q.point, b), q.direction}),
                       constant(r.sq(offset_radius)));
    } else {
      const Quadratic cross{
          {p.cross(v, relative.point), p.cross(v, q.direction), 0}};
      comparison =
          sub(mul(cross, cross), constant(r.mul(r.sq(offset_radius), squared)));
    }
    if (sign(comparison) < 0) {
      return true;
    }
  }
  return false;
}
bool CircularArrangement::contains(Probe q) {
  if (clip) {
    if (sign(sub(coordinate(q, false), constant(box[0]))) < 0 ||
        sign(sub(constant(box[2]), coordinate(q, false))) < 0 ||
        sign(sub(coordinate(q, true), constant(box[1]))) < 0 ||
        sign(sub(constant(box[3]), coordinate(q, true))) < 0) {
      return false;
    }
  }
  if (offset_mode) {
    const bool inside = inside_original(q);
    if (!r.sign(offset_radius)) {
      return inside;
    }
    return erosion ? inside && !near_original(q) : inside || near_original(q);
  }
  for (const auto& shape : shapes) {
    if (inside_shape(shape, q)) {
      return true;
    }
  }
  return false;
}
void CircularArrangement::cut(bool circular, std::uint32_t support,
                              std::uint32_t v) {
  event();
  cuts.push_back({circular, support, v});
}
bool CircularArrangement::on_line(std::uint32_t index, Point value) {
  const auto edge = lines[index];
  const auto a = points[edge.a], b = points[edge.b], v = p.sub(b, a);
  return !r.sign(p.cross(v, p.sub(value, a))) &&
         r.sign(p.dot(p.sub(value, a), v)) >= 0 &&
         r.sign(p.dot(p.sub(value, b), v)) <= 0;
}
void CircularArrangement::intersect_lines(std::uint32_t i, std::uint32_t j) {
  const auto a = points[lines[i].a], b = points[lines[i].b];
  const auto c = points[lines[j].a], d = points[lines[j].b];
  const auto u = p.sub(b, a), v = p.sub(d, c), delta = p.sub(c, a);
  const auto denominator = p.cross(u, v);
  if (!r.sign(denominator)) {
    if (r.sign(p.cross(u, delta))) {
      return;
    }
    for (auto x : {lines[i].a, lines[i].b}) {
      if (on_line(j, points[x])) {
        cut(false, j, x);
      }
    }
    for (auto x : {lines[j].a, lines[j].b}) {
      if (on_line(i, points[x])) {
        cut(false, i, x);
      }
    }
    return;
  }
  const auto t = r.div(p.cross(delta, v), denominator);
  const auto s = r.div(p.cross(delta, u), denominator);
  if (r.sign(t) < 0 || r.compare(t, 1) > 0 || r.sign(s) < 0 ||
      r.compare(s, 1) > 0) {
    return;
  }
  const auto id = vertex(p.add(a, p.mul(u, t)));
  cut(false, i, id);
  cut(false, j, id);
}
void CircularArrangement::intersect_line_circle(std::uint32_t i,
                                                std::uint32_t j) {
  const auto a = points[lines[i].a], b = points[lines[i].b];
  const auto circle = circles[j];
  const auto v = p.sub(b, a), x = p.sub(a, circle.center);
  const auto aa = p.norm(v), bb = p.dot(x, v);
  const auto cc = r.sub(p.norm(x), r.sq(circle.radius));
  const auto discriminant = r.sub(r.sq(bb), r.mul(aa, cc));
  const int sign_d = r.sign(discriminant);
  if (sign_d < 0) {
    return;
  }
  const auto root = r.sqrt(discriminant);
  for (int side : {-1, 1}) {
    if (side == 1 && !sign_d) {
      break;
    }
    const auto t = r.div(r.add(r.neg(bb), r.mul(r.integer(side), root)), aa);
    if (r.sign(t) < 0 || r.compare(t, 1) > 0) {
      continue;
    }
    const auto id = vertex(p.add(a, p.mul(v, t)));
    cut(false, i, id);
    cut(true, j, id);
  }
}
void CircularArrangement::intersect_circles(std::uint32_t i, std::uint32_t j) {
  const auto a = circles[i], b = circles[j];
  const auto v = p.sub(b.center, a.center);
  const auto squared = p.norm(v);
  if (!r.sign(squared)) {
    return;
  }
  const auto h = r.div(r.add(squared, r.sub(r.sq(a.radius), r.sq(b.radius))),
                       r.mul(r.integer(2), squared));
  const auto height = r.sub(r.div(r.sq(a.radius), squared), r.sq(h));
  const int sign_h = r.sign(height);
  if (sign_h < 0) {
    return;
  }
  const auto middle = p.add(a.center, p.mul(v, h)),
             delta = p.mul(p.perp(v), r.sqrt(height));
  for (int side : {-1, 1}) {
    if (side == 1 && !sign_h) {
      break;
    }
    const auto id = vertex(p.add(middle, p.mul(delta, r.integer(side))));
    cut(true, i, id);
    cut(true, j, id);
  }
}
CircularArrangement::Point CircularArrangement::arc_sample(const Edge& edge) {
  const auto circle = circles[edge.circle];
  Point a = p.sub(points[edge.ccw ? edge.a : edge.b], circle.center);
  Point b = p.sub(points[edge.ccw ? edge.b : edge.a], circle.center);
  const int sx = r.sign(a.x), sy = r.sign(a.y);
  const unsigned quadrant = sy > 0   ? (sx > 0 ? 0 : 1)
                            : sy < 0 ? (sx < 0 ? 2 : 3)
                            : sx > 0 ? 0
                                     : 2;
  const auto rotate = [&](Point v, unsigned turns) {
    for (unsigned j = 0; j < turns; ++j) {
      v = p.perp(v);
    }
    return v;
  };
  a = rotate(a, (4 - quadrant) % 4);
  b = rotate(b, (4 - quadrant) % 4);
  auto low = r.m.exact.integer(0), high = r.m.exact.integer(1);
  for (unsigned attempt = 0; attempt < r.m.max_refinements; ++attempt) {
    const auto middle = r.m.div(r.m.add(low, high), r.m.exact.integer(2));
    const auto t = r.constant(middle);
    const int lower = r.sign(r.sub(a.y, r.mul(t, r.add(circle.radius, a.x))));
    const int upper = r.sign(r.sub(b.y, r.mul(t, r.add(circle.radius, b.x))));
    if (lower < 0 && upper > 0) {
      const auto denominator = r.add(1, r.sq(t));
      Point unit{r.div(r.sub(1, r.sq(t)), denominator),
                 r.div(r.mul(r.integer(2), t), denominator)};
      return p.add(circle.center, p.mul(rotate(unit, quadrant), circle.radius));
    }
    if (lower >= 0) {
      low = middle;
    } else {
      high = middle;
    }
  }
  throw Status{ErrorCode::OperationFailed,
               "rational circular sample search unresolved",
               DomainErrorCode{UINT64_C(0x47454e0001)}};
}
CircularArrangement::Point CircularArrangement::arc_middle(const Edge& edge) {
  const auto circle = circles[edge.circle];
  const auto sum = p.add(p.sub(points[edge.a], circle.center),
                         p.sub(points[edge.b], circle.center));
  return p.add(circle.center,
               p.mul(sum, r.div(circle.radius, r.sqrt(p.norm(sum)))));
}
void CircularArrangement::select(Edge edge) {
  Point sample, normal;
  if (edge.circle < 0) {
    sample = p.mul(p.add(points[edge.a], points[edge.b]),
                   r.constant(r.m.exact.fraction(1, 2)));
    normal = p.perp(p.sub(points[edge.b], points[edge.a]));
  } else {
    sample = arc_sample(edge);
    normal = p.sub(circles[edge.circle].center, sample);
  }
  const bool left = contains({sample, normal});
  const bool right = contains({sample, p.mul(normal, r.integer(-1))});
  if (left == right) {
    return;
  }
  if (!left) {
    std::swap(edge.a, edge.b);
    edge.ccw = false;
  }
  for (const auto& old : boundary) {
    if (old.a == edge.a && old.b == edge.b && old.circle == edge.circle &&
        (edge.circle < 0 || old.ccw == edge.ccw)) {
      return;
    }
  }
  event();
  boundary.push_back(edge);
}
void CircularArrangement::arrange() {
  cuts.clear();
  boundary.clear();
  cycles.clear();
  cycle_offsets.clear();
  for (std::uint32_t j = 0; j < lines.size(); ++j) {
    cut(false, j, lines[j].a);
    cut(false, j, lines[j].b);
    for (std::uint32_t i = 0; i < j; ++i) {
      event();
      intersect_lines(i, j);
    }
    for (std::uint32_t i = 0; i < circles.size(); ++i) {
      event();
      intersect_line_circle(j, i);
    }
  }
  for (std::uint32_t j = 0; j < circles.size(); ++j) {
    const auto c = circles[j];
    for (const auto direction :
         {Point{1, 0}, Point{0, 1}, Point{r.integer(-1), 0},
          Point{0, r.integer(-1)}}) {
      cut(true, j, vertex(p.add(c.center, p.mul(direction, c.radius))));
    }
    for (std::uint32_t i = 0; i < j; ++i) {
      event();
      intersect_circles(i, j);
    }
  }
  std::sort(cuts.begin(), cuts.end(), [&](const Cut& a, const Cut& b) {
    if (a.circular != b.circular) {
      return a.circular < b.circular;
    }
    if (a.support != b.support) {
      return a.support < b.support;
    }
    if (a.vertex == b.vertex) {
      return false;
    }
    if (a.circular) {
      const auto center = circles[a.support].center;
      return p.angle(p.sub(points[a.vertex], center),
                     p.sub(points[b.vertex], center)) < 0;
    }
    const auto line = lines[a.support];
    const auto direction = p.sub(points[line.b], points[line.a]);
    return r.sign(p.dot(p.sub(points[a.vertex], points[b.vertex]), direction)) <
           0;
  });
  cuts.erase(std::unique(cuts.begin(), cuts.end(),
                         [](const Cut& a, const Cut& b) {
                           return a.circular == b.circular &&
                                  a.support == b.support &&
                                  a.vertex == b.vertex;
                         }),
             cuts.end());
  for (std::size_t begin = 0; begin < cuts.size();) {
    std::size_t end = begin + 1;
    while (end < cuts.size() && cuts[end].circular == cuts[begin].circular &&
           cuts[end].support == cuts[begin].support) {
      ++end;
    }
    for (std::size_t j = begin; j + 1 < end; ++j) {
      select({cuts[j].vertex, cuts[j + 1].vertex,
              cuts[j].circular ? cuts[j].support : -1LL, true});
    }
    if (cuts[begin].circular) {
      select({cuts[end - 1].vertex, cuts[begin].vertex, cuts[begin].support,
              true});
    }
    begin = end;
  }
}
CircularArrangement::Point CircularArrangement::tangent(const Edge& edge,
                                                        bool at_end,
                                                        bool reverse) {
  Point direction;
  if (edge.circle < 0) {
    direction = p.sub(points[edge.b], points[edge.a]);
  } else {
    direction = p.perp(
        p.sub(points[at_end ? edge.b : edge.a], circles[edge.circle].center));
    if (!edge.ccw) {
      direction = p.mul(direction, r.integer(-1));
    }
  }
  return reverse ? p.mul(direction, r.integer(-1)) : direction;
}
CircularArrangement::Id CircularArrangement::curvature(const Edge& edge,
                                                       bool reverse) {
  if (edge.circle < 0) {
    return 0;
  }
  auto value = r.inverse(circles[edge.circle].radius);
  return edge.ccw != reverse ? value : r.neg(value);
}
void CircularArrangement::trace() {
  cycles.clear();
  cycle_offsets.clear();
  std::pmr::vector<std::uint32_t> next(boundary.size(), UINT32_MAX, resource),
      path(resource), starts(resource);
  std::pmr::vector<std::uint8_t> used(boundary.size(), 0, resource);
  const auto angle_less = [&](Point a, Id ka, Point b, Id kb) {
    const int order = p.angle(a, b);
    return order ? order < 0 : r.compare(ka, kb) < 0;
  };
  for (std::uint32_t i = 0; i < boundary.size(); ++i) {
    const auto reverse = tangent(boundary[i], true, true);
    const auto kr = curvature(boundary[i], true);
    std::uint32_t lower = UINT32_MAX, largest = UINT32_MAX;
    for (std::uint32_t j = 0; j < boundary.size(); ++j) {
      if (boundary[j].a != boundary[i].b) {
        continue;
      }
      const auto tj = tangent(boundary[j], false);
      const auto kj = curvature(boundary[j]);
      if (largest == UINT32_MAX ||
          angle_less(tangent(boundary[largest], false),
                     curvature(boundary[largest]), tj, kj)) {
        largest = j;
      }
      if (angle_less(tj, kj, reverse, kr) &&
          (lower == UINT32_MAX ||
           angle_less(tangent(boundary[lower], false),
                      curvature(boundary[lower]), tj, kj))) {
        lower = j;
      }
    }
    next[i] = lower == UINT32_MAX ? largest : lower;
    if (next[i] == UINT32_MAX) {
      throw Status{ErrorCode::OperationFailed,
                   "circular boundary construction did not close"};
    }
  }
  for (std::uint32_t first = 0; first < boundary.size(); ++first) {
    if (used[first]) {
      continue;
    }
    path.clear();
    auto at = first;
    do {
      event();
      if (used[at]) {
        throw Status{ErrorCode::OperationFailed,
                     "circular boundary rotation is not a permutation"};
      }
      used[at] = 1;
      path.push_back(at);
      at = next[at];
    } while (at != first);
    const auto rotation_less = [&](std::uint32_t a, std::uint32_t b) {
      for (std::size_t j = 0; j < path.size(); ++j) {
        const auto& x = boundary[path[(a + j) % path.size()]];
        const auto& y = boundary[path[(b + j) % path.size()]];
        const auto order = p.compare(points[x.a], points[y.a]);
        if (order) {
          return order < 0;
        }
        if (x.circle != y.circle) {
          return x.circle < y.circle;
        }
      }
      return false;
    };
    std::uint32_t start = 0;
    for (std::uint32_t j = 1; j < path.size(); ++j) {
      if (rotation_less(j, start)) {
        start = j;
      }
    }
    starts.push_back(static_cast<std::uint32_t>(cycles.size()));
    for (std::size_t j = 0; j < path.size(); ++j) {
      cycles.push_back(path[(start + j) % path.size()]);
    }
  }
  starts.push_back(static_cast<std::uint32_t>(cycles.size()));
  std::pmr::vector<std::uint32_t> order(resource), sorted(resource);
  for (std::uint32_t j = 0; j + 1 < starts.size(); ++j) {
    order.push_back(j);
  }
  std::sort(order.begin(), order.end(), [&](auto a, auto b) {
    const auto na = starts[a + 1] - starts[a], nb = starts[b + 1] - starts[b];
    for (std::uint32_t j = 0; j < std::min(na, nb); ++j) {
      const auto value = p.compare(points[boundary[cycles[starts[a] + j]].a],
                                   points[boundary[cycles[starts[b] + j]].a]);
      if (value) {
        return value < 0;
      }
    }
    return na < nb;
  });
  for (auto c : order) {
    cycle_offsets.push_back(static_cast<std::uint32_t>(sorted.size()));
    sorted.insert(sorted.end(), cycles.begin() + starts[c],
                  cycles.begin() + starts[c + 1]);
  }
  cycle_offsets.push_back(static_cast<std::uint32_t>(sorted.size()));
  cycles.swap(sorted);
}
int CircularArrangement::along(const Edge& edge, Point direction) {
  const auto center = circles[edge.circle].center;
  const auto a = p.sub(points[edge.ccw ? edge.a : edge.b], center);
  const auto b = p.sub(points[edge.ccw ? edge.b : edge.a], center);
  return r.sign(p.cross(a, direction)) >= 0 &&
         r.sign(p.cross(direction, b)) >= 0;
}
CircularArrangement::Id CircularArrangement::distance_squared(Point query) {
  Id best = 0;
  bool found = false;
  for (const auto& edge : boundary) {
    const auto a = points[edge.a], b = points[edge.b];
    Id distance;
    if (edge.circle < 0) {
      const auto v = p.sub(b, a), q = p.sub(query, a);
      const auto t = r.max(0, r.min(1, r.div(p.dot(q, v), p.norm(v))));
      distance = p.norm(p.sub(q, p.mul(v, t)));
    } else {
      const auto circle = circles[edge.circle];
      const auto q = p.sub(query, circle.center);
      if (!r.sign(p.norm(q)) || along(edge, q)) {
        distance = r.sq(r.sub(r.sqrt(p.norm(q)), circle.radius));
      } else {
        distance = r.min(p.norm(p.sub(query, a)), p.norm(p.sub(query, b)));
      }
    }
    if (!found || r.compare(distance, best) < 0) {
      best = distance;
      found = true;
    }
  }
  return best;
}
OwnedInterval CircularArrangement::area(unsigned precision) {
  auto sum = r.im.integer(0);
  Id algebraic = 0;
  for (const auto& edge : boundary) {
    const auto a = points[edge.a], b = points[edge.b];
    if (edge.circle < 0) {
      algebraic = r.add(algebraic, p.cross(a, b));
      continue;
    }
    const auto circle = circles[edge.circle];
    algebraic = r.add(algebraic, p.cross(circle.center, p.sub(b, a)));
    const auto u = p.sub(points[edge.ccw ? edge.a : edge.b], circle.center);
    const auto v = p.sub(points[edge.ccw ? edge.b : edge.a], circle.center);
    const auto x = p.dot(u, v), y = p.cross(u, v);
    const int xzero = r.sign(x), yzero = r.sign(y), order = r.compare(y, x);
    const auto xx = r.enclosure(x, precision), yy = r.enclosure(y, precision);
    OwnedInterval angle;
    if (!yzero) {
      angle = r.im.integer(0);
    } else if (!xzero) {
      r.m.d.constant(angle.view(), true);
      angle = r.im.scale(angle, -1);
    } else {
      r.m.atan_positive(angle.view(), yy.view(), xx.view(), order);
    }
    auto term = r.im.mul(r.enclosure(r.sq(circle.radius), precision), angle);
    if (!edge.ccw) {
      term = r.im.negate(term);
    }
    sum = r.im.add(sum, term);
  }
  sum = r.im.add(sum, r.enclosure(algebraic, precision));
  r.im.check_state();
  return r.im.scale(sum, -1);
}
std::uint64_t CircularArrangement::rounded_area(bool narrow) {
  bool arcs = false;
  Id exact = 0;
  for (const auto& edge : boundary) {
    arcs |= edge.circle >= 0;
    if (edge.circle < 0) {
      exact = r.add(exact, p.cross(points[edge.a], points[edge.b]));
    }
  }
  if (!arcs) {
    return r.round(r.div(exact, r.integer(2)), narrow);
  }
  for (unsigned precision = 128, attempt = 0;
       precision <= 4096 && attempt < r.m.max_refinements;
       precision *= 2, ++attempt) {
    r.m.d.reset();
    r.m.d.precision = precision;
    try {
      const auto interval = area(precision);
      std::uint64_t answer;
      if (r.im.rounded(interval, narrow, &answer)) {
        return answer;
      }
    } catch (const IntegrationRetry&) {
    }
  }
  throw Status{ErrorCode::OperationFailed, "circular area rounding unresolved",
               DomainErrorCode{UINT64_C(0x47454e0001)}};
}
}  // namespace ps::plugin_internal::generation
