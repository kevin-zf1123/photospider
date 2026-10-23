#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "04-mask-morphology/spatial.hpp"

namespace ps::plugin_internal::mask {
enum class MetricKind { L1, L2, Linf };
struct Site final {
  std::int64_t y = 0, x = 0;
};
inline bool site_less(Site a, Site b) {
  return a.y < b.y || (a.y == b.y && a.x < b.x);
}
inline std::uint64_t separation(std::int64_t a, std::int64_t b) {
  return static_cast<std::uint64_t>(a < b ? b - a : a - b);
}
struct Metric final {
  MetricKind kind;
  double sy, sx;
  bool isotropic;
  explicit Metric(const Parameters& p)
      : sy(real(p, "sy")), sx(real(p, "sx")), isotropic(sy == sx) {
    const auto name = text(p, "metric");
    parameter(name == "l1" || name == "l2" || name == "linf",
              "unknown distance metric");
    kind = name == "l1"   ? MetricKind::L1
           : name == "l2" ? MetricKind::L2
                          : MetricKind::Linf;
  }
  unsigned __int128 integer_key(Site p, Site q) const {
    const auto dy = separation(p.y, q.y), dx = separation(p.x, q.x);
    if (kind == MetricKind::L1) {
      return dy + dx;
    }
    if (kind == MetricKind::Linf) {
      return std::max(dy, dx);
    }
    return static_cast<unsigned __int128>(dy) * dy +
           static_cast<unsigned __int128>(dx) * dx;
  }
  Fast enclosure(Site p, Site q) const {
    const auto y = Fast::point(static_cast<double>(separation(p.y, q.y))) *
                   Fast::point(sy);
    const auto x = Fast::point(static_cast<double>(separation(p.x, q.x))) *
                   Fast::point(sx);
    if (kind == MetricKind::L1) {
      return y + x;
    }
    if (kind == MetricKind::Linf) {
      return {std::max(y.low, x.low), std::max(y.high, x.high)};
    }
    return square(y) + square(x);
  }
  Rational exact(Exact& e, Site p, Site q) const {
    const auto y =
        e.multiply(e.integer(separation(p.y, q.y)), e.binary(bits(sy)));
    const auto x =
        e.multiply(e.integer(separation(p.x, q.x)), e.binary(bits(sx)));
    if (kind == MetricKind::L1) {
      return e.add(y, x);
    }
    if (kind == MetricKind::Linf) {
      return e.order(y, x) >= 0 ? y : x;
    }
    return e.add(e.multiply(y, y), e.multiply(x, x));
  }
  int compare(Exact& e, Site p, Site a, Site b) const {
    if (isotropic) {
      const auto ak = integer_key(p, a), bk = integer_key(p, b);
      return ak < bk ? -1 : ak > bk ? 1 : 0;
    }
    // Symmetry is exact, including identical lower bounds at kd partitions.
    if (separation(p.y, a.y) == separation(p.y, b.y) &&
        separation(p.x, a.x) == separation(p.x, b.x)) {
      return 0;
    }
    const auto aa = enclosure(p, a), bb = enclosure(p, b);
    if (aa.finite() && bb.finite()) {
      if (aa.high < bb.low) {
        return -1;
      }
      if (aa.low > bb.high) {
        return 1;
      }
    }
    const auto result = e.order(exact(e, p, a), exact(e, p, b));
    check(e.status());
    return result;
  }
  bool within(Exact& e, Site p, Site q, double radius) const {
    if (p.y == q.y && p.x == q.x) {
      return true;
    }
    if (isotropic && sy == 1) {
      const auto key = integer_key(p, q);
      if (kind != MetricKind::L2) {
        return static_cast<double>(key) <= radius;
      }
      if (radius <= 0x1p26 && radius == std::floor(radius)) {
        const auto r = static_cast<std::uint64_t>(radius);
        return key <= static_cast<unsigned __int128>(r) * r;
      }
    }
    const auto distance = enclosure(p, q);
    const auto r = kind == MetricKind::L2 ? square(Fast::point(radius))
                                          : Fast::point(radius);
    if (distance.finite() && r.finite()) {
      if (distance.high <= r.low) {
        return true;
      }
      if (distance.low > r.high) {
        return false;
      }
    }
    auto bound = e.binary(bits(radius));
    if (kind == MetricKind::L2) {
      bound = e.multiply(bound, bound);
    }
    const auto result = e.order(exact(e, p, q), bound);
    check(e.status());
    return result <= 0;
  }
  std::uint64_t rounded_distance(Exact& e, Site p, Site q, bool squared,
                                 bool narrow) const {
    if (p.y == q.y && p.x == q.x) {
      return 0;
    }
    if (isotropic) {
      const auto key = integer_key(p, q);
      if (kind != MetricKind::L2 && !narrow) {
        return bits(static_cast<double>(key) * sy);
      }
      if (sy == 1 && key <= (static_cast<unsigned __int128>(1) << 53)) {
        const auto value = static_cast<double>(key);
        if (kind != MetricKind::L2 || squared) {
          return convert(value, narrow);
        }
        const auto root = std::sqrt(value);
        if (!narrow) {
          return bits(root);
        }
        // Compare exact integer squares with the adjacent Float32 midpoints.
        // The midpoint has <=25 significant bits and its square fits uint128.
        const auto candidate = convert(root, true);
        const auto compare_midpoint = [&](std::uint64_t a, std::uint64_t b) {
          const auto midpoint = (numeric_ops::numeric_double(a, true) +
                                 numeric_ops::numeric_double(b, true)) *
                                .5;
          const auto parts =
              numeric_ops::BinaryParts::decode(bits(midpoint), false);
          auto square = static_cast<unsigned __int128>(parts.significand) *
                        parts.significand;
          auto integer = key;
          if (parts.exponent >= 0) {
            square <<= 2 * parts.exponent;
          } else {
            // binary64 imports have trailing zero bits; remove them first.
            const unsigned trailing =
                static_cast<unsigned>(__builtin_ctzll(parts.significand));
            square >>= 2 * trailing;
            const int exponent = parts.exponent + static_cast<int>(trailing);
            if (exponent >= 0) {
              square <<= 2 * exponent;
            } else {
              integer <<= -2 * exponent;
            }
          }
          return integer < square ? -1 : integer > square ? 1 : 0;
        };
        const auto below = compare_midpoint(candidate - 1, candidate);
        if (below < 0 || (!below && (candidate & 1))) {
          return candidate - 1;
        }
        const auto above = compare_midpoint(candidate, candidate + 1);
        if (above > 0 || (!above && (candidate & 1))) {
          return candidate + 1;
        }
        return candidate;
      }
    }
    auto interval = enclosure(p, q);
    if (kind == MetricKind::L2 && !squared && interval.finite()) {
      interval = {
          numeric_ops::numeric_down(std::sqrt(std::max(0.0, interval.low))),
          numeric_ops::numeric_up(std::sqrt(std::max(0.0, interval.high)))};
    }
    if (const auto result = certified(interval, narrow)) {
      return *result;
    }
    const auto value = exact(e, p, q);
    const auto result = kind == MetricKind::L2 && !squared
                            ? e.rounded_sqrt(value, narrow)
                            : e.rounded(value, narrow);
    check(e.status());
    return result;
  }
};
struct SiteNode final {
  Site point;
  std::int64_t ymin, xmin, ymax, xmax;
};
struct NearestSite final {
  Site site{};
  bool found = false;
};
struct SiteTree final {
  SiteNode* nodes;
  std::uint64_t count;
  const Metric& metric;
  Exact& exact;
  Visits& visits;
  void build(std::uint64_t first, std::uint64_t last, unsigned depth = 0) {
    if (first == last) {
      return;
    }
    const auto mid = first + (last - first) / 2;
    std::nth_element(nodes + first, nodes + mid, nodes + last,
                     [&](const SiteNode& a, const SiteNode& b) {
                       visits.step();
                       return depth % 2 ? (a.point.x == b.point.x
                                               ? a.point.y < b.point.y
                                               : a.point.x < b.point.x)
                                        : site_less(a.point, b.point);
                     });
    build(first, mid, depth + 1);
    build(mid + 1, last, depth + 1);
    auto& n = nodes[mid];
    n.ymin = n.ymax = n.point.y;
    n.xmin = n.xmax = n.point.x;
    const auto merge = [&](const SiteNode& child) {
      n.ymin = std::min(n.ymin, child.ymin);
      n.xmin = std::min(n.xmin, child.xmin);
      n.ymax = std::max(n.ymax, child.ymax);
      n.xmax = std::max(n.xmax, child.xmax);
    };
    if (first != mid) {
      merge(nodes[first + (mid - first) / 2]);
    }
    if (mid + 1 != last) {
      merge(nodes[mid + 1 + (last - mid - 1) / 2]);
    }
    visits.step();
  }
  void search(Site query, std::uint64_t first, std::uint64_t last,
              NearestSite* best) const {
    if (first == last) {
      return;
    }
    visits.step();
    const auto mid = first + (last - first) / 2;
    const auto& n = nodes[mid];
    if (best->found) {
      const Site lower{std::clamp(query.y, n.ymin, n.ymax),
                       std::clamp(query.x, n.xmin, n.xmax)};
      const auto order = metric.compare(exact, query, lower, best->site);
      if (order > 0 ||
          (order == 0 && !site_less({n.ymin, n.xmin}, best->site))) {
        return;
      }
    }
    const auto order =
        best->found ? metric.compare(exact, query, n.point, best->site) : -1;
    if (order < 0 || (order == 0 && site_less(n.point, best->site))) {
      *best = {n.point, true};
    }
    const auto left = first == mid ? mid : first + (mid - first) / 2;
    const auto right = mid + 1 == last ? mid : mid + 1 + (last - mid - 1) / 2;
    const auto lower = [&](std::uint64_t index) {
      const auto& b = nodes[index];
      return Site{std::clamp(query.y, b.ymin, b.ymax),
                  std::clamp(query.x, b.xmin, b.xmax)};
    };
    const bool right_first =
        mid + 1 != last &&
        (first == mid ||
         metric.compare(exact, query, lower(right), lower(left)) < 0);
    if (right_first) {
      search(query, mid + 1, last, best);
      search(query, first, mid, best);
    } else {
      search(query, first, mid, best);
      search(query, mid + 1, last, best);
    }
  }
  NearestSite nearest(Site query) const {
    NearestSite result;
    search(query, 0, count, &result);
    return result;
  }
};
}  // namespace ps::plugin_internal::mask
