#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "03-generation/radical.hpp"

namespace ps::plugin_internal::generation {
// Rational-circle / radical-line arrangement. Arc order and incidence are
// exact; infinitesimal side tests select the boundary of the regular closed
// region.
class CircularArrangement final {
 public:
  using Id = Radical::Id;
  using Point = RadicalPoint;
  struct Line {
    std::uint32_t a, b;
  };
  struct Circle {
    Point center;
    Id radius;
  };
  struct Edge {
    std::uint32_t a, b;
    std::int64_t circle = -1;
    bool ccw = true;
  };
  struct Cut {
    bool circular;
    std::uint32_t support, vertex;
  };
  struct Shape {
    enum Kind { Polygon, Disk, Hull, Sector } kind = Polygon;
    std::uint32_t begin = 0, count = 0;
    Point a, b;
    Id radius_a = 0, radius_b = 0, squared = 0, delta = 0, k = 0;
  };
  struct Probe {
    Point point, direction;
  };
  struct Quadratic {
    Id c[3]{};
  };
  Radical& r;
  RadicalPlane p;
  std::pmr::memory_resource* resource;
  std::pmr::vector<Point> points, polygon_points;
  std::pmr::vector<std::uint32_t> point_order;
  std::pmr::vector<Line> lines, original;
  std::pmr::vector<Circle> circles;
  std::pmr::vector<Shape> shapes;
  std::pmr::vector<Cut> cuts;
  std::pmr::vector<Edge> boundary;
  std::pmr::vector<std::uint32_t> cycles, cycle_offsets;
  std::uint64_t maximum, events = 0;
  bool offset_mode = false, erosion = false, clip = false;
  Id offset_radius = 0;
  std::array<Id, 4> box{};
  CircularArrangement(Radical& arithmetic, std::pmr::memory_resource* memory,
                      std::uint64_t max_events);
  void event(std::uint64_t count = 1);
  std::uint32_t vertex(Point value);
  void line(Point a, Point b);
  void circle(Point center, Id radius);
  void polygon(const Point* values, std::size_t count);
  void disk(Point center, Id radius);
  void sector(Point center, Id radius, Point start, Point end);
  void hull(Point a, Point b, Id ra, Id rb);
  void offset_edge(Point a, Point b, Id radius);
  void arrange();
  void trace();
  bool contains(Probe q);
  Point arc_sample(const Edge& edge);
  Point arc_middle(const Edge& edge);
  Point tangent(const Edge& edge, bool at_end, bool reverse = false);
  Id curvature(const Edge& edge, bool reverse = false);
  Id distance_squared(Point query);
  OwnedInterval area(unsigned precision);
  std::uint64_t rounded_area(bool narrow);
  int along(const Edge& edge, Point direction);
  Quadratic constant(Id value) { return {{value, 0, 0}}; }
  Quadratic add(Quadratic a, Quadratic b);
  Quadratic sub(Quadratic a, Quadratic b);
  Quadratic mul(Quadratic a, Quadratic b);
  Quadratic coordinate(Probe q, bool y);
  Quadratic dot_probe(Point v, Probe q);
  Quadratic norm_probe(Probe q);
  int sign(Quadratic value);
  bool inside_shape(const Shape& shape, Probe q);
  bool inside_original(Probe q);
  bool near_original(Probe q);
  void intersect_lines(std::uint32_t a, std::uint32_t b);
  void intersect_line_circle(std::uint32_t line, std::uint32_t circle);
  void intersect_circles(std::uint32_t a, std::uint32_t b);
  void cut(bool circular, std::uint32_t support, std::uint32_t v);
  bool on_line(std::uint32_t line, Point v);
  void select(Edge edge);
};
}  // namespace ps::plugin_internal::generation
