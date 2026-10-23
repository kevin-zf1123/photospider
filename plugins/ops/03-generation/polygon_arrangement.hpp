#pragma once

#include <cstdint>
#include <memory_resource>
#include <vector>

#include "03-generation/path_support.hpp"

namespace ps::plugin_internal::generation {
struct ExactPoint final {
  Rational x, y;
};
struct ExactEdge final {
  std::uint64_t a, b;
  unsigned source;
};
struct ExactContour final {
  std::uint64_t begin, end;
};
enum class BooleanKind { Union, Intersection, Difference, Xor };
struct PolygonArrangement final {
  Math& m;
  std::pmr::memory_resource* resource;
  std::uint64_t max_events;
  std::pmr::vector<ExactPoint> points;
  std::pmr::vector<std::uint64_t> sorted_points;
  std::pmr::vector<ExactEdge> input_edges, boundary;
  std::pmr::vector<std::uint64_t> vertices;
  std::pmr::vector<ExactContour> contours;
  PolygonArrangement(Math& math, std::pmr::memory_resource* memory,
                     std::uint64_t events);
  int compare(const ExactPoint&, const ExactPoint&) const;
  bool equal(const ExactPoint&, const ExactPoint&) const;
  ExactPoint sub(const ExactPoint&, const ExactPoint&) const;
  ExactPoint lerp(const ExactPoint&, const ExactPoint&, const Rational&) const;
  Rational cross(const ExactPoint&, const ExactPoint&) const;
  Rational dot(const ExactPoint&, const ExactPoint&) const;
  std::uint64_t point(ExactPoint);
  void edge(std::uint64_t a, std::uint64_t b, unsigned source);
  void read(const Reader&, unsigned port, bool grid = false,
            double grid_spacing = 1, double origin_x = 0, double origin_y = 0,
            Rational* quantization_squared = nullptr);
  void build(BooleanKind kind, bool evenodd_a, bool evenodd_b);
  void canonicalize();
  bool inside(const ExactPoint&, unsigned source, bool evenodd) const;
  Rational squared_distance(const ExactPoint&,
                            ExactPoint* closest = nullptr) const;
};
BooleanKind boolean_kind(const Parameters&);
bool fill_evenodd(const Parameters&, const char* key = "fill_rule");
Rational read_rational(const Reader&, unsigned port, unsigned signs,
                       unsigned sizes, unsigned limbs, std::uint64_t row,
                       unsigned component, Math&);
void write_rational(const Dense&, unsigned signs, unsigned sizes,
                    unsigned limbs, std::uint64_t row, unsigned component,
                    std::uint64_t capacity, const Rational&);
std::int64_t round_integer(Math&, const Rational&);
double upward_sqrt(Math&, const Rational&);
}  // namespace ps::plugin_internal::generation
