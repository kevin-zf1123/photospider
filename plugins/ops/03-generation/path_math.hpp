#pragma once

#include <utility>

#include "03-generation/paths.hpp"

namespace ps::plugin_internal::generation {
enum class CurveKind { Bezier, Hermite, Arc, FullTurn, Spline, Close };
struct Curve final {
  CurveKind kind = CurveKind::Bezier;
  std::uint64_t row = 0;
  unsigned field = 0, degree = 1;
  bool pooled = false, core = false;
  std::array<std::uint64_t, 4> control{};
};
struct CurveValue final {
  std::array<Rational, 2> position, derivative;
  std::array<bool, 2> copy_position{}, copy_derivative{};
};
struct EvaluationScratch final {
  Math math;
  model_ops::RationalMathStorage<32> compact{
      numeric_ops::SequenceProfile::Strict};
  std::array<Rational, 17> basis, lower, left, right;
  explicit EvaluationScratch(const Work& work) : math(work) {
    compact.bind(work);
  }
};
Curve core_curve(const Reader&, Math&, std::uint64_t row);
Curve primitive_curve(const Reader&, std::uint64_t row);
class CurveMath final {
 public:
  const Reader& reader;
  EvaluationScratch& scratch;
  Math& m;
  CurveMath(const Reader& source, EvaluationScratch& workspace)
      : reader(source), scratch(workspace), m(workspace.math) {}
  bool rational(const Curve& curve, const Rational& t, bool derivative,
                CurveValue* output);
  std::uint64_t sample(const Curve& curve, const Rational& t, unsigned output,
                       unsigned axis, bool narrow);
  bool valid(const Curve& curve, const Rational& t);

 private:
  Rational control(const Curve&, unsigned point, unsigned axis) const;
  bool compact_polynomial(const Curve&, const Rational&, bool, CurveValue*);
  void bezier(const Curve&, const Rational&, bool, CurveValue*);
  void hermite(const Curve&, const Rational&, bool, CurveValue*);
  void spline(const Curve&, const Rational&, bool, CurveValue*);
  void basis(const Curve&, std::uint64_t knot_begin, std::uint64_t span,
             unsigned degree, const Rational& tau,
             std::array<Rational, 17>* output);
  Rational knot(const Curve&, std::uint64_t row) const;
  std::pair<Curve, Curve> closure(const Curve&) const;
  bool endpoints_equal(const Curve& last, const Curve& first);
  bool arc_landmark(const Curve&, const Rational&, int* sine, int* cosine);
  void arc_vector(const Curve&, const Rational&, Interval output, unsigned axis,
                  bool derivative, bool unit_speed = false);
  void component(const Curve&, const Rational&, Interval output, unsigned axis,
                 bool derivative);
};
}  // namespace ps::plugin_internal::generation
