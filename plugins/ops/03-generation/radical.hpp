#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory_resource>
#include <unordered_map>
#include <utility>
#include <vector>

#include "03-generation/interval_support.hpp"

namespace ps::plugin_internal::generation {
// Exact real radical expressions. Predicates first use directed enclosures;
// unresolved zero is proved by the algebraic-integer norm separation bound.
// All nodes, constants, caches and traversal storage use admitted host scratch.
class Radical final {
 public:
  using Id = std::uint32_t;
  enum class Kind : std::uint8_t {
    Constant,
    Add,
    Multiply,
    Negate,
    Inverse,
    Sqrt
  };
  struct Fast {
    double low = -std::numeric_limits<double>::infinity();
    double high = std::numeric_limits<double>::infinity();
  };
  static bool finite(Fast x) {
    return std::isfinite(x.low) && std::isfinite(x.high);
  }
  static Fast outward(double low, double high) {
    if (!std::isfinite(low) || !std::isfinite(high)) {
      return {};
    }
    Fast result{std::nextafter(low, -std::numeric_limits<double>::infinity()),
                std::nextafter(high, std::numeric_limits<double>::infinity())};
    return finite(result) ? result : Fast{};
  }
  struct Node {
    Kind kind = Kind::Constant;
    Id a = 0, b = 0;
    unsigned denominator_bits = 0, conjugate_bits = 0;
    std::uint64_t hash = 0;
    int known_sign = 2;
    Fast fast;
  };
  struct Cache {
    OwnedInterval value;
    unsigned precision = 0;
  };
  struct Mark {
    std::size_t nodes, constants;
  };
  Math& m;
  IntervalMath im;
  std::pmr::deque<Node> nodes;
  std::pmr::deque<Rational> constants;
  std::pmr::deque<Cache> cache;
  std::pmr::unordered_multimap<std::uint64_t, Id> interned;
  std::pmr::vector<Id> pending, active;
  std::pmr::vector<std::uint64_t> seen;
  std::uint64_t generation = 0, maximum;
  explicit Radical(Math& arithmetic, std::pmr::memory_resource* resource,
                   std::uint64_t max_nodes = 1048576)
      : m(arithmetic),
        im(m),
        nodes(resource),
        constants(resource),
        cache(resource),
        interned(resource),
        pending(resource),
        active(resource),
        seen(resource),
        maximum(max_nodes) {
    constant(m.exact.integer(0));
    constant(m.exact.integer(1));
  }
  static unsigned plus(unsigned a, unsigned b) {
    return a >= 65536 || b >= 65536 || a + b >= 65536 ? 65536 : a + b;
  }
  static unsigned product(unsigned a, unsigned b) {
    return a && b > 65536 / a ? 65536 : a * b;
  }
  static std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
    return a ^ (b + UINT64_C(0x9e3779b97f4a7c15) + (a << 6) + (a >> 2));
  }
  static unsigned ceil_log(const Exact::Integer& n) {
    const int top = Exact::Workspace::top(n);
    if (top <= 0) {
      return 0;
    }
    bool power = true;
    for (unsigned i = 0; i < (static_cast<unsigned>(top) + 64) / 64; ++i) {
      if (n.words[i] != (i == static_cast<unsigned>(top) / 64
                             ? UINT64_C(1) << (top % 64)
                             : 0)) {
        power = false;
      }
    }
    return static_cast<unsigned>(top) + !power;
  }
  Id canonical(Id value) const {
    return nodes[value].known_sign == 0 ? 0 : value;
  }
  bool rational(Id value) const { return nodes[value].kind == Kind::Constant; }
  const Rational& fraction(Id value) const { return constants[nodes[value].a]; }
  Fast filter(const Node& node) {
    if (node.kind == Kind::Constant) {
      const auto& value = constants[node.a];
      const auto rounded = number(m.rounded(value, false));
      if (!std::isfinite(rounded)) {
        return {};
      }
      const int order = m.exact.order(m.value(rounded), value);
      Fast result{
          order > 0 ? std::nextafter(rounded,
                                     -std::numeric_limits<double>::infinity())
                    : rounded,
          order < 0
              ? std::nextafter(rounded, std::numeric_limits<double>::infinity())
              : rounded};
      return finite(result) ? result : Fast{};
    }
    const auto a = nodes[node.a].fast, b = nodes[node.b].fast;
    if (!finite(a) ||
        ((node.kind == Kind::Add || node.kind == Kind::Multiply) &&
         !finite(b))) {
      return {};
    }
    if (node.kind == Kind::Negate) {
      return {-a.high, -a.low};
    }
    if (node.kind == Kind::Add) {
      return outward(a.low + b.low, a.high + b.high);
    }
    if (node.kind == Kind::Inverse) {
      if (a.low <= 0 && a.high >= 0) {
        return {};
      }
      return outward(1 / a.high, 1 / a.low);
    }
    if (node.kind == Kind::Sqrt) {
      if (a.high < 0) {
        return {};
      }
      return outward(std::sqrt(std::max(0.0, a.low)), std::sqrt(a.high));
    }
    const double products[] = {a.low * b.low, a.low * b.high, a.high * b.low,
                               a.high * b.high};
    double low = products[0], high = products[0];
    for (double value : products) {
      if (!std::isfinite(value)) {
        return {};
      }
      low = std::min(low, value);
      high = std::max(high, value);
    }
    return outward(low, high);
  }
  Id append(Node node) {
    m.work.poll();
    if (nodes.size() >= maximum || nodes.size() >= UINT32_MAX) {
      throw Status{ErrorCode::ResourceExhausted,
                   "radical expression node capacity",
                   FailureReason::CapacityLimit};
    }
    const auto id = static_cast<Id>(nodes.size());
    node.fast = filter(node);
    nodes.push_back(node);
    cache.emplace_back();
    seen.push_back(0);
    interned.emplace(node.hash, id);
    return id;
  }
  Id constant(const Rational& value) {
    check(m.exact.status());
    auto hash = mix(0, value.negative && !Exact::zero(value));
    for (const auto* n : {&value.n, &value.d}) {
      const auto count = (Exact::Workspace::top(*n) + 64) / 64;
      hash = mix(hash, count);
      for (int j = 0; j < count; ++j) {
        hash = mix(hash, n->words[j]);
      }
    }
    const auto range = interned.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      if (rational(it->second) &&
          m.exact.order(fraction(it->second), value) == 0) {
        return it->second;
      }
    }
    Node node;
    node.a = static_cast<Id>(constants.size());
    node.denominator_bits = ceil_log(value.d);
    node.conjugate_bits = std::max(
        0, Exact::Workspace::top(value.n) - Exact::Workspace::top(value.d) + 1);
    node.known_sign = Exact::zero(value) ? 0 : value.negative ? -1 : 1;
    node.hash = hash;
    constants.push_back(value);
    return append(node);
  }
  Id integer(std::int64_t value) { return constant(m.exact.integer(value)); }
  Id value(double number) { return constant(m.value(number)); }
  Id input(const Reader& reader, unsigned port, unsigned member,
           const Coordinate& at) {
    return constant(m.input(reader, port, member, at));
  }
  Id node(Kind kind, Id a, Id b = 0) {
    const auto hash = mix(mix(static_cast<unsigned>(kind), a), b);
    const auto range = interned.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      const auto& q = nodes[it->second];
      if (q.kind == kind && q.a == a && q.b == b) {
        return canonical(it->second);
      }
    }
    Node result;
    result.kind = kind;
    result.a = a;
    result.b = b;
    result.hash = hash;
    const auto& x = nodes[a];
    const auto& y = nodes[b];
    if (kind == Kind::Add) {
      result.denominator_bits = plus(x.denominator_bits, y.denominator_bits);
      result.conjugate_bits =
          plus(std::max(x.conjugate_bits, y.conjugate_bits), 1);
    } else if (kind == Kind::Multiply) {
      result.denominator_bits = plus(x.denominator_bits, y.denominator_bits);
      result.conjugate_bits = plus(x.conjugate_bits, y.conjugate_bits);
    } else if (kind == Kind::Sqrt) {
      result.denominator_bits = x.denominator_bits;
      result.conjugate_bits =
          x.conjugate_bits >= 65536 ? 65536 : (x.conjugate_bits + 1) / 2;
      result.known_sign = 1;
    } else if (kind == Kind::Inverse) {
      const auto degree = degree_bound(a);
      result.denominator_bits =
          product(degree, plus(x.denominator_bits, x.conjugate_bits));
      result.conjugate_bits = plus(product(degree, x.denominator_bits),
                                   product(degree - 1, x.conjugate_bits));
      result.known_sign = x.known_sign;
    } else {
      result.denominator_bits = x.denominator_bits;
      result.conjugate_bits = x.conjugate_bits;
      if (x.known_sign != 2) {
        result.known_sign = -x.known_sign;
      }
    }
    return append(result);
  }
  Id neg(Id a) {
    a = canonical(a);
    if (!a) {
      return 0;
    }
    if (rational(a)) {
      return constant(m.exact.negate(fraction(a)));
    }
    if (nodes[a].kind == Kind::Negate) {
      return nodes[a].a;
    }
    return node(Kind::Negate, a);
  }
  Id add(Id a, Id b) {
    a = canonical(a);
    b = canonical(b);
    if (!a) {
      return b;
    }
    if (!b) {
      return a;
    }
    if (rational(a) && rational(b)) {
      return constant(m.add(fraction(a), fraction(b)));
    }
    if ((nodes[a].kind == Kind::Negate && nodes[a].a == b) ||
        (nodes[b].kind == Kind::Negate && nodes[b].a == a)) {
      return 0;
    }
    if (a == b) {
      return mul(integer(2), a);
    }
    if (a > b) {
      std::swap(a, b);
    }
    return node(Kind::Add, a, b);
  }
  Id sub(Id a, Id b) { return a == b ? 0 : add(a, neg(b)); }
  Id mul(Id a, Id b) {
    a = canonical(a);
    b = canonical(b);
    if (!a || !b) {
      return 0;
    }
    if (a == 1) {
      return b;
    }
    if (b == 1) {
      return a;
    }
    if (rational(a) && rational(b)) {
      return constant(m.mul(fraction(a), fraction(b)));
    }
    if (nodes[a].kind == Kind::Negate) {
      return neg(mul(nodes[a].a, b));
    }
    if (nodes[b].kind == Kind::Negate) {
      return neg(mul(a, nodes[b].a));
    }
    if (a == b && nodes[a].kind == Kind::Sqrt) {
      return nodes[a].a;
    }
    if (nodes[a].kind == Kind::Inverse && nodes[a].a == b) {
      return 1;
    }
    if (nodes[b].kind == Kind::Inverse && nodes[b].a == a) {
      return 1;
    }
    if (rational(b)) {
      std::swap(a, b);
    }
    if (rational(a) && nodes[b].kind == Kind::Multiply &&
        rational(nodes[b].a)) {
      return mul(mul(a, nodes[b].a), nodes[b].b);
    }
    if (a > b) {
      std::swap(a, b);
    }
    return node(Kind::Multiply, a, b);
  }
  Id sq(Id a) { return mul(a, a); }
  Id inverse(Id a) {
    a = canonical(a);
    if (rational(a)) {
      return constant(m.div(m.exact.integer(1), fraction(a)));
    }
    if (nodes[a].kind == Kind::Inverse) {
      return nodes[a].a;
    }
    if (nodes[a].kind == Kind::Negate) {
      return neg(inverse(nodes[a].a));
    }
    if (!sign(a)) {
      throw Status{ErrorCode::OperationFailed, "zero algebraic denominator"};
    }
    if (nodes[a].kind == Kind::Sqrt) {
      return div(a, nodes[a].a);
    }
    return node(Kind::Inverse, a);
  }
  Id div(Id a, Id b) {
    if (!canonical(a)) {
      return 0;
    }
    if (a == b) {
      return 1;
    }
    return mul(a, inverse(b));
  }
  Id sqrt(Id a) {
    a = canonical(a);
    if (!a || a == 1) {
      return a;
    }
    if (rational(a)) {
      const auto root = m.rounded_sqrt(fraction(a), false);
      const auto candidate = m.exact.binary(root);
      if (Exact::finite(candidate) &&
          m.exact.order(m.sq(candidate), fraction(a)) == 0) {
        return constant(candidate);
      }
    }
    const int s = sign(a);
    if (!s) {
      return 0;
    }
    if (s < 0) {
      throw Status{ErrorCode::OperationFailed, "negative algebraic radicand"};
    }
    if (nodes[a].kind == Kind::Multiply && nodes[a].a == nodes[a].b) {
      const auto v = nodes[a].a;
      return sign(v) < 0 ? neg(v) : v;
    }
    return node(Kind::Sqrt, a);
  }
  unsigned visit(Id root) {
    ++generation;
    pending.clear();
    pending.push_back(root);
    active.clear();
    unsigned radicals = 0;
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (seen[id] == generation) {
        continue;
      }
      seen[id] = generation;
      active.push_back(id);
      m.work.poll();
      const auto& q = nodes[id];
      if (q.kind == Kind::Constant || q.known_sign == 0) {
        continue;
      }
      if (q.kind == Kind::Sqrt) {
        ++radicals;
      }
      pending.push_back(q.a);
      if (q.kind == Kind::Add || q.kind == Kind::Multiply) {
        pending.push_back(q.b);
      }
    }
    return radicals;
  }
  unsigned degree_bound(Id root) {
    const unsigned n = visit(root);
    return n >= 16 ? 65536 : 1U << n;
  }
  unsigned separation_bits(Id root) {
    const auto degree = degree_bound(root);
    const auto& q = nodes[root];
    return plus(product(degree, q.denominator_bits),
                product(degree - 1, q.conjugate_bits));
  }
  OwnedInterval enclosure(Id root, unsigned precision) {
    if (m.d.precision != precision || !m.d.active()) {
      m.d.reset();
      m.d.precision = precision;
    }
    if (cache[root].precision == precision) {
      return cache[root].value;
    }
    visit(root);
    std::sort(active.begin(), active.end());
    for (const auto id : active) {
      if (cache[id].precision == precision) {
        continue;
      }
      const auto q = nodes[id];
      auto& out = cache[id];
      if (q.known_sign == 0) {
        out.value = im.integer(0);
      } else if (q.kind == Kind::Constant) {
        out.value = im.rational(fraction(id));
      } else if (q.kind == Kind::Add) {
        out.value = im.add(cache[q.a].value, cache[q.b].value);
      } else if (q.kind == Kind::Multiply) {
        out.value = im.mul(cache[q.a].value, cache[q.b].value);
      } else if (q.kind == Kind::Negate) {
        out.value = im.negate(cache[q.a].value);
      } else if (q.kind == Kind::Inverse) {
        out.value = im.div(im.integer(1), cache[q.a].value);
      } else {
        out.value = im.sqrt(cache[q.a].value);
      }
      im.check_state();
      out.precision = precision;
    }
    return cache[root].value;
  }
  int sign(Id root) {
    if (nodes[root].known_sign != 2) {
      return nodes[root].known_sign;
    }
    // Nearest/gradual-underflow scope and disabled contraction are required.
    // Outward binary64 intervals prove strict signs only. Equality and every
    // published coordinate still use the directed exact certificate below.
    if (nodes[root].fast.low > 0) {
      return nodes[root].known_sign = 1;
    }
    if (nodes[root].fast.high < 0) {
      return nodes[root].known_sign = -1;
    }
    const unsigned bound = separation_bits(root);
    for (unsigned p = 128, attempt = 0;
         p <= 4096 && attempt < m.max_refinements; p *= 2, ++attempt) {
      try {
        const auto x = enclosure(root, p);
        int result = 2;
        if (im.positive(x.low)) {
          result = 1;
        } else if (x.high.negative &&
                   Directed::Workspace::top(x.high.magnitude) >= 0) {
          result = -1;
        } else if (m.d.compare(x.low, x.high) == 0 &&
                   Directed::Workspace::top(x.low.magnitude) < 0) {
          result = 0;
        } else if (bound < p) {
          const auto limit = im.scale(im.integer(1), -static_cast<int>(bound));
          const auto negative = im.negate(limit);
          if (m.d.compare(x.low, negative.low) > 0 &&
              m.d.compare(x.high, limit.high) < 0) {
            result = 0;
          }
        }
        if (result != 2) {
          nodes[root].known_sign = result;
          if (!result) {
            cache[root].precision = 0;
          }
          return result;
        }
      } catch (const IntegrationRetry&) {
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "radical predicate certification unresolved",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  int compare(Id a, Id b) {
    if (a == b) {
      return 0;
    }
    if (rational(a) && rational(b)) {
      return m.exact.order(fraction(a), fraction(b));
    }
    if (nodes[a].fast.high < nodes[b].fast.low) {
      return -1;
    }
    if (nodes[a].fast.low > nodes[b].fast.high) {
      return 1;
    }
    return sign(sub(a, b));
  }
  Id absolute(Id a) { return sign(a) < 0 ? neg(a) : a; }
  Id min(Id a, Id b) { return compare(a, b) <= 0 ? a : b; }
  Id max(Id a, Id b) { return compare(a, b) >= 0 ? a : b; }
  std::uint64_t round(Id root, bool narrow = false) {
    root = canonical(root);
    if (rational(root)) {
      return m.rounded(fraction(root), narrow);
    }
    for (unsigned p = 128, attempt = 0;
         p <= 4096 && attempt < m.max_refinements; p *= 2, ++attempt) {
      try {
        const auto x = enclosure(root, p);
        std::uint64_t answer;
        if (im.rounded(x, narrow, &answer)) {
          return answer;
        }
        const auto a = m.d.rounded(x.low, narrow),
                   b = m.d.rounded(x.high, narrow);
        const auto mask = UINT64_C(1) << (narrow ? 31 : 63);
        const auto ordered = [mask](std::uint64_t q) {
          return q & mask ? ~q : q | mask;
        };
        const auto exponent =
            narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
        if (!(a & ~mask) && !(b & ~mask)) {
          return sign(root) < 0 ? mask : 0;
        }
        if ((a & exponent) != exponent && (b & exponent) != exponent &&
            ordered(b) - ordered(a) == 1) {
          const auto boundary = constant(
              m.div(m.add(m.exact.binary(a, narrow), m.exact.binary(b, narrow)),
                    m.exact.integer(2)));
          const int order = compare(root, boundary);
          return !order ? (a & 1 ? b : a) : order < 0 ? a : b;
        }
      } catch (const IntegrationRetry&) {
      }
    }
    throw Status{ErrorCode::OperationFailed,
                 "radical rounding certification unresolved",
                 DomainErrorCode{UINT64_C(0x47454e0001)}};
  }
  Mark mark() const { return {nodes.size(), constants.size()}; }
  void rewind(Mark saved) {
    while (nodes.size() > saved.nodes) {
      const auto id = static_cast<Id>(nodes.size() - 1);
      const auto range = interned.equal_range(nodes.back().hash);
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second == id) {
          interned.erase(it);
          break;
        }
      }
      nodes.pop_back();
      cache.pop_back();
      seen.pop_back();
    }
    while (constants.size() > saved.constants) {
      constants.pop_back();
    }
    pending.clear();
  }
};
struct RadicalPoint final {
  Radical::Id x = 0, y = 0;
};
class RadicalPlane final {
 public:
  Radical& r;
  explicit RadicalPlane(Radical& arithmetic) : r(arithmetic) {}
  RadicalPoint add(RadicalPoint a, RadicalPoint b) {
    return {r.add(a.x, b.x), r.add(a.y, b.y)};
  }
  RadicalPoint sub(RadicalPoint a, RadicalPoint b) {
    return {r.sub(a.x, b.x), r.sub(a.y, b.y)};
  }
  RadicalPoint mul(RadicalPoint a, Radical::Id b) {
    return {r.mul(a.x, b), r.mul(a.y, b)};
  }
  RadicalPoint perp(RadicalPoint a) { return {r.neg(a.y), a.x}; }
  Radical::Id dot(RadicalPoint a, RadicalPoint b) {
    return r.add(r.mul(a.x, b.x), r.mul(a.y, b.y));
  }
  Radical::Id cross(RadicalPoint a, RadicalPoint b) {
    return r.sub(r.mul(a.x, b.y), r.mul(a.y, b.x));
  }
  Radical::Id norm(RadicalPoint a) { return dot(a, a); }
  int compare(RadicalPoint a, RadicalPoint b) {
    const int x = r.compare(a.x, b.x);
    return x ? x : r.compare(a.y, b.y);
  }
  int angle(RadicalPoint a, RadicalPoint b) {
    const auto half = [&](RadicalPoint v) {
      const int y = r.sign(v.y);
      return y > 0 || (!y && r.sign(v.x) >= 0) ? 0 : 1;
    };
    const auto ha = half(a), hb = half(b);
    return ha != hb ? (ha < hb ? -1 : 1) : -r.sign(cross(a, b));
  }
};
}  // namespace ps::plugin_internal::generation
