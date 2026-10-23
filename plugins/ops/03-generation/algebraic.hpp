#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "03-generation/math.hpp"

namespace ps::plugin_internal::generation {
struct Polynomial final {
  std::array<Rational, 7> c{};
  int degree = -1;
};
struct RationalBounds final {
  Rational low, high;
};
struct AlgebraicRoot final {
  Polynomial equation;
  Rational low, high;
  int variation_low = 0, variation_high = 0;
  unsigned refinements = 0;
  bool exact = true;
};
struct AlgebraicWorkspace final {
  struct Pending {
    Rational a, b;
    int va = 0, vb = 0;
  };
  std::array<std::array<Rational, 6>, 6> matrix{}, product{}, next{};
  std::array<Polynomial, 32> polynomials;
  std::array<Pending, 7> pending;
  unsigned used = 0;
};
// Degree six covers squared cubic distance. All polynomial and matrix
// temporaries are in admitted scratch, not the worker's bounded native stack.
class Algebra final {
  struct Frame final {
    AlgebraicWorkspace& storage;
    unsigned saved;
    explicit Frame(AlgebraicWorkspace& s) : storage(s), saved(s.used) {}
    ~Frame() { storage.used = saved; }
    Polynomial& polynomial() {
      if (storage.used == storage.polynomials.size())
        throw Status{ErrorCode::ResourceExhausted,
                     "algebraic temporary capacity",
                     FailureReason::CapacityLimit};
      return storage.polynomials[storage.used++];
    }
  };

 public:
  Math& m;
  AlgebraicWorkspace& storage;
  explicit Algebra(Math& arithmetic, AlgebraicWorkspace& arena)
      : m(arithmetic), storage(arena) {}
  void clear(Polynomial* p) {
    p->degree = -1;
    for (auto& v : p->c)
      v = Rational{};
  }
  void trim(Polynomial* p) {
    while (p->degree >= 0 && Exact::zero(p->c[p->degree]))
      --p->degree;
    check(m.exact.status());
  }
  void constant(Polynomial* p, const Rational& value) {
    clear(p);
    p->c[0] = value;
    p->degree = Exact::zero(value) ? -1 : 0;
  }
  Rational evaluate(const Polynomial& p, const Rational& t) {
    auto value = m.exact.integer(0);
    for (int j = p.degree; j >= 0; --j)
      value = m.add(m.mul(value, t), p.c[j]);
    return value;
  }
  RationalBounds bounds(const Polynomial& p, const Rational& a,
                        const Rational& b) {
    RationalBounds value{};
    for (int j = p.degree; j >= 0; --j) {
      const std::array<Rational, 4> q{m.mul(value.low, a), m.mul(value.low, b),
                                      m.mul(value.high, a),
                                      m.mul(value.high, b)};
      value.low = value.high = q[0];
      for (unsigned k = 1; k < 4; ++k) {
        value.low = m.min(value.low, q[k]);
        value.high = m.max(value.high, q[k]);
      }
      value.low = m.add(value.low, p.c[j]);
      value.high = m.add(value.high, p.c[j]);
    }
    return value;
  }
  void derivative(Polynomial* out, const Polynomial& p) {
    clear(out);
    out->degree = std::max(-1, p.degree - 1);
    for (int j = 1; j <= p.degree; ++j)
      out->c[j - 1] = m.scale(p.c[j], j);
    trim(out);
  }
  void add(Polynomial* out, const Polynomial& a, const Polynomial& b) {
    const int degree = std::max(a.degree, b.degree);
    for (int j = 0; j <= degree; ++j)
      out->c[j] = m.add(j <= a.degree ? a.c[j] : Rational{},
                        j <= b.degree ? b.c[j] : Rational{});
    out->degree = degree;
    trim(out);
  }
  void multiply(Polynomial* out, const Polynomial& a, const Polynomial& b) {
    clear(out);
    if (a.degree < 0 || b.degree < 0)
      return;
    if (a.degree + b.degree > 6)
      throw Status{ErrorCode::ResourceExhausted,
                   "algebraic polynomial degree budget",
                   FailureReason::CapacityLimit};
    out->degree = a.degree + b.degree;
    for (int i = 0; i <= a.degree; ++i)
      for (int j = 0; j <= b.degree; ++j)
        out->c[i + j] = m.add(out->c[i + j], m.mul(a.c[i], b.c[j]));
    trim(out);
  }
  void remainder(Polynomial* out, const Polynomial& a, const Polynomial& b,
                 Polynomial* quotient = nullptr) {
    if (out != &a)
      *out = a;
    if (quotient)
      clear(quotient);
    while (out->degree >= b.degree && out->degree >= 0) {
      m.work.poll();
      const int delta = out->degree - b.degree;
      const auto scale = m.div(out->c[out->degree], b.c[b.degree]);
      if (quotient) {
        quotient->c[delta] = scale;
        quotient->degree = std::max(quotient->degree, delta);
      }
      for (int j = 0; j <= b.degree; ++j)
        out->c[j + delta] = m.sub(out->c[j + delta], m.mul(scale, b.c[j]));
      trim(out);
    }
  }
  void monic(Polynomial* p) {
    if (p->degree >= 0) {
      const auto lead = p->c[p->degree];
      for (int j = 0; j <= p->degree; ++j)
        p->c[j] = m.div(p->c[j], lead);
    }
  }
  void gcd(Polynomial* out, const Polynomial& first, const Polynomial& second) {
    Frame frame(storage);
    auto* a = &frame.polynomial();
    auto* b = &frame.polynomial();
    auto* next = &frame.polynomial();
    *a = first;
    *b = second;
    while (b->degree >= 0) {
      remainder(next, *a, *b);
      std::swap(a, b);
      std::swap(b, next);
    }
    *out = *a;
    monic(out);
  }
  void squarefree(Polynomial* out, const Polynomial& p) {
    if (p.degree <= 0) {
      *out = p;
      return;
    }
    Frame frame(storage);
    auto& change = frame.polynomial();
    auto& common = frame.polynomial();
    auto& residual = frame.polynomial();
    derivative(&change, p);
    gcd(&common, p, change);
    remainder(&residual, p, common, out);
    monic(out);
  }
  int variations(const Polynomial& equation, const Rational& t) {
    if (equation.degree <= 0)
      return 0;
    Frame frame(storage);
    auto* a = &frame.polynomial();
    auto* b = &frame.polynomial();
    auto* next = &frame.polynomial();
    *a = equation;
    derivative(b, equation);
    int result = 0, previous = 0;
    for (;;) {
      const auto at = evaluate(*a, t);
      const int sign = Exact::zero(at) ? 0 : at.negative ? -1 : 1;
      if (sign) {
        result += previous && previous != sign;
        previous = sign;
      }
      if (b->degree < 0)
        break;
      remainder(next, *a, *b);
      for (int j = 0; j <= next->degree; ++j)
        next->c[j] = m.exact.negate(next->c[j]);
      std::swap(a, b);
      std::swap(b, next);
    }
    return result;
  }
  void rational(AlgebraicRoot* r, const Rational& value) {
    r->low = r->high = value;
    r->exact = true;
    r->refinements = 0;
  }
  void refine(AlgebraicRoot* r) {
    if (r->exact)
      return;
    if (++r->refinements > m.max_refinements)
      throw Status{ErrorCode::OperationFailed,
                   "algebraic isolation refinement limit",
                   DomainErrorCode{UINT64_C(0x47454e0001)}};
    m.work.poll();
    const auto t = m.div(m.add(r->low, r->high), m.exact.integer(2));
    if (Exact::zero(evaluate(r->equation, t))) {
      rational(r, t);
      return;
    }
    const auto v = variations(r->equation, t);
    if (r->variation_low - v) {
      r->high = t;
      r->variation_high = v;
    } else {
      r->low = t;
      r->variation_low = v;
    }
  }
  unsigned roots(const Polynomial& polynomial, const Rational& a,
                 const Rational& b, std::array<AlgebraicRoot, 6>* result) {
    Frame frame(storage);
    auto& p = frame.polynomial();
    squarefree(&p, polynomial);
    if (p.degree <= 0)
      return 0;
    auto& pending = storage.pending;
    unsigned pending_count = 1, count = 0, splits = 0;
    pending[0] = {a, b, variations(p, a), variations(p, b)};
    // Sturm differences count (a,b]. The caller separately includes a.
    while (pending_count) {
      const auto part = pending[--pending_count];
      if (!(part.va - part.vb))
        continue;
      if (part.va - part.vb == 1) {
        auto& r = (*result)[count++];
        r.equation = p;
        r.low = part.a;
        r.high = part.b;
        r.variation_low = part.va;
        r.variation_high = part.vb;
        r.refinements = 0;
        r.exact = Exact::zero(evaluate(p, part.b));
        if (r.exact)
          r.low = r.high;
        continue;
      }
      if (++splits > m.max_refinements)
        throw Status{ErrorCode::OperationFailed,
                     "stationary root isolation limit",
                     DomainErrorCode{UINT64_C(0x47454e0001)}};
      const auto mid = m.div(m.add(part.a, part.b), m.exact.integer(2));
      const auto v = variations(p, mid);
      if (v - part.vb)
        pending[pending_count++] = {mid, part.b, v, part.vb};
      if (part.va - v)
        pending[pending_count++] = {part.a, mid, part.va, v};
    }
    return count;
  }
  int compare(AlgebraicRoot* a, const Rational& b) {
    for (;;) {
      if (m.exact.order(a->high, b) < 0)
        return -1;
      if (m.exact.order(a->low, b) > 0)
        return 1;
      if (a->exact)
        return m.exact.order(a->low, b);
      if (m.exact.order(a->low, b) == 0)
        return 1;
      if (Exact::zero(evaluate(a->equation, b)))
        return 0;
      refine(a);
    }
  }
  int compare(AlgebraicRoot* a, AlgebraicRoot* b) {
    if (a->exact)
      return -compare(b, a->low);
    if (b->exact)
      return compare(a, b->low);
    Frame frame(storage);
    auto& common = frame.polynomial();
    gcd(&common, a->equation, b->equation);
    for (;;) {
      if (m.exact.order(a->high, b->low) <= 0)
        return -1;
      if (m.exact.order(b->high, a->low) <= 0)
        return 1;
      if (common.degree > 0) {
        const auto low = m.max(a->low, b->low), high = m.min(a->high, b->high);
        if (variations(common, low) - variations(common, high))
          return 0;
      }
      refine(a);
      refine(b);
      if (a->exact)
        return -compare(b, a->low);
      if (b->exact)
        return compare(a, b->low);
    }
  }
  void characteristic(Polynomial* out, const Polynomial& equation,
                      const Polynomial& value) {
    Frame frame(storage);
    auto& column = frame.polynomial();
    auto& answer = frame.polynomial();
    const int n = equation.degree;
    auto& matrix = storage.matrix;
    auto& b = storage.product;
    auto& next = storage.next;
    for (auto* values : {&matrix, &b, &next})
      for (auto& row : *values)
        for (auto& v : row)
          v = Rational{};
    remainder(&column, value, equation);
    for (int j = 0; j < n; ++j) {
      for (int i = 0; i <= column.degree; ++i)
        matrix[i][j] = column.c[i];
      for (int i = column.degree; i >= 0; --i)
        column.c[i + 1] = column.c[i];
      column.c[0] = Rational{};
      if (column.degree >= 0)
        ++column.degree;
      remainder(&column, column, equation);
      b[j][j] = m.exact.integer(1);
    }
    clear(&answer);
    answer.degree = n;
    answer.c[n] = m.exact.integer(1);
    // Faddeev-LeVerrier over Q, applied to multiplication by value in Q[t]/p.
    for (int k = 1; k <= n; ++k) {
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
          next[i][j] = Rational{};
          for (int h = 0; h < n; ++h)
            next[i][j] = m.add(next[i][j], m.mul(matrix[i][h], b[h][j]));
        }
      auto trace = m.exact.integer(0);
      for (int i = 0; i < n; ++i)
        trace = m.add(trace, next[i][i]);
      answer.c[n - k] = m.div(m.exact.negate(trace), m.exact.integer(k));
      for (int i = 0; i < n; ++i)
        next[i][i] = m.add(next[i][i], answer.c[n - k]);
      b = next;
    }
    squarefree(out, answer);
  }
  void project(AlgebraicRoot* out, AlgebraicRoot* input,
               const Polynomial& expression) {
    if (input->exact) {
      rational(out, evaluate(expression, input->low));
      return;
    }
    Frame frame(storage);
    auto& value = frame.polynomial();
    auto& equation = frame.polynomial();
    remainder(&value, expression, input->equation);
    if (value.degree <= 0) {
      rational(out, value.c[0]);
      return;
    }
    characteristic(&equation, input->equation, value);
    for (;;) {
      auto interval = bounds(value, input->low, input->high);
      if (m.exact.order(interval.low, interval.high) == 0) {
        rational(out, interval.low);
        return;
      }
      const auto radius = m.sub(interval.high, interval.low);
      interval.low = m.sub(interval.low, radius);
      interval.high = m.add(interval.high, radius);
      const auto va = variations(equation, interval.low);
      const auto vb = variations(equation, interval.high);
      if (va - vb == 1) {
        out->equation = equation;
        out->low = interval.low;
        out->high = interval.high;
        out->variation_low = va;
        out->variation_high = vb;
        out->refinements = 0;
        out->exact = false;
        return;
      }
      refine(input);
      if (input->exact) {
        rational(out, evaluate(expression, input->low));
        return;
      }
    }
  }
  std::uint64_t round(AlgebraicRoot* root, bool narrow,
                      bool square_root = false) {
    if (root->exact)
      return square_root ? m.rounded_sqrt(root->low, narrow)
                         : m.rounded(root->low, narrow);
    for (;;) {
      const auto low = square_root ? m.positive(root->low) : root->low;
      const auto high = square_root ? m.positive(root->high) : root->high;
      const auto a =
          square_root ? m.rounded_sqrt(low, narrow) : m.rounded(low, narrow);
      const auto b =
          square_root ? m.rounded_sqrt(high, narrow) : m.rounded(high, narrow);
      if (a == b)
        return a;
      const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
      if (!(a & ~sign) && !(b & ~sign)) {
        const int side = compare(root, m.exact.integer(0));
        return side < 0 ? sign : 0;
      }
      const auto ordered = [sign](std::uint64_t x) {
        return x & sign ? ~x : x | sign;
      };
      const auto exponent =
          narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
      if ((a & exponent) != exponent && (b & exponent) != exponent &&
          ordered(b) - ordered(a) == 1) {
        const auto boundary =
            m.div(m.add(m.exact.binary(a, narrow), m.exact.binary(b, narrow)),
                  m.exact.integer(2));
        const int side = compare(root, square_root ? m.sq(boundary) : boundary);
        if (!side)
          return a & 1 ? b : a;
        return side < 0 ? a : b;
      }
      refine(root);
      if (root->exact)
        return square_root ? m.rounded_sqrt(root->low, narrow)
                           : m.rounded(root->low, narrow);
    }
  }
};
}  // namespace ps::plugin_internal::generation
