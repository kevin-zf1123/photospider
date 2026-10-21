#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/model_math.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mapping_ops {
using namespace color_point;  // NOLINT(build/namespaces)
using Math = model_ops::RationalMath;
using Rational = Math::Rational;
using Fast = numeric_ops::FastInterval;
using Bits = std::array<std::uint64_t, 4>;
enum class Kind { Clip, Reduce, Compress, Check, Tone, Display };
struct Constants final {
  std::array<Rational, 3> weights;
  std::array<Fast, 3> fast_weights;
  Fast fast_white_squared, fast_range, fast_knee_complement;
  std::optional<basis_ops::ExactRow> affine;
  Rational gain, white_squared, knee, black, range;
  std::vector<basis_ops::ExactRow> forward, inverse;
  model_ops::MathConfig lab_config, xyz_config;
  std::shared_ptr<const model_ops::Constants> lab, xyz;
};
struct State final : Plan {
  Kind kind = Kind::Clip;
  bool extended = false, signed_luminance = false, to_nits = true;
  double gain = 1, white = 1, knee = .8, black = 0, white_nits = 1;
  std::shared_ptr<const Constants> constants;
};
Rational imported(const basis_ops::Rational& value) {
  Rational out;
  out.n.words.fill(0);
  out.d.words.fill(0);
  const auto used = [](const auto& words) {
    auto size = words.size();
    while (size && !words[size - 1]) {
      --size;
    }
    return size;
  };
  const auto numerator = used(value.n.words), denominator = used(value.d.words);
  if (numerator > 2 * out.n.words.size() ||
      denominator > 2 * out.d.words.size()) {
    throw basis_ops::CheckedFailure{
        Status{ErrorCode::ResourceExhausted,
               "color coefficient exceeds exact arithmetic capacity",
               FailureReason::CapacityLimit}};
  }
  for (std::size_t i = 0; i < numerator; ++i) {
    out.n.words[i / 2] |= static_cast<std::uint64_t>(value.n.words[i])
                          << ((i % 2) * 32);
  }
  for (std::size_t i = 0; i < denominator; ++i) {
    out.d.words[i / 2] |= static_cast<std::uint64_t>(value.d.words[i])
                          << ((i % 2) * 32);
  }
  out.negative = value.negative;
  return out;
}
std::uint64_t one(bool narrow) {
  return narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
}
bool in_cube(const Bits& input, bool narrow) {
  for (unsigned c = 0; c < 3; ++c) {
    const double v = numeric_ops::numeric_double(input[c], narrow);
    if (v < 0 || v > 1) {
      return false;
    }
  }
  return true;
}
std::uint64_t clipped(std::uint64_t bits, bool narrow) {
  const double v = numeric_ops::numeric_double(bits, narrow);
  return v < 0 ? 0 : v > 1 ? one(narrow) : bits;
}
struct RationalWork final {
  basis_ops::RowWorkspace row;
  Math math{numeric_ops::SequenceProfile::Strict};
  explicit RationalWork(Math::Work consume) { math.bind(std::move(consume)); }
};
struct ReductionWork final {
  Math math{numeric_ops::SequenceProfile::Strict};
  basis_ops::RowWorkspace row;
  basis_ops::CandidateBlock candidate;
  model_ops::ModelMath lab, xyz;
  ReductionWork(const Constants& c, Math::Work consume)
      : lab(c.lab_config, *c.lab, consume), xyz(c.xyz_config, *c.xyz, consume) {
    math.bind(std::move(consume));
  }
};
std::uint64_t rounded(Math& math, Rational value, bool narrow) {
  if (Math::zero(value)) {
    value.negative = false;
  }
  return math.rounded(value, narrow);
}
std::optional<std::uint64_t> strict_fast(Fast value, bool narrow) {
  if (!value.finite() || !value.nonzero()) {
    return {};
  }
  const auto a = numeric_ops::numeric_bits(value.low, narrow);
  const auto b = numeric_ops::numeric_bits(value.high, narrow);
  if (a == b && basis_ops::finite_bits(a, narrow)) {
    return a;
  }
  return {};
}
bool fast_rational(const State& s, const Bits& input, unsigned requested,
                   Bits* output) {
#ifdef PHOTOSPIDER_FMT_MAPPING_REFERENCE
  (void)s;
  (void)input;
  (void)requested;
  (void)output;
  return false;
#else
  // Distinct binary64 enclosure endpoints cannot certify one binary64 value.
  // F64 compression still benefits from certified endpoint/copy branches.
  if (!s.narrow && s.kind != Kind::Compress) {
    return false;
  }
  const auto& c = *s.constants;
  const auto point = Fast::point;
  const auto count = static_cast<unsigned>(s.selection.channels.size());
  std::array<Fast, 3> x{};
  for (unsigned i = 0; i < count; ++i) {
    x[i] = point(numeric_ops::numeric_double(input[i], s.narrow));
  }
  std::array<Fast, 3> result{};
  if (s.kind == Kind::Display) {
    const Fast range = c.fast_range;
    for (unsigned i = 0; i < count; ++i) {
      if (requested & (1U << i)) {
        result[i] = s.to_nits ? point(s.black) + range * x[i]
                              : (x[i] - point(s.black)) / range;
      }
    }
  } else {
    Fast y = count == 1 ? x[0]
                        : c.fast_weights[0] * x[0] + c.fast_weights[1] * x[1] +
                              c.fast_weights[2] * x[2];
    if (!y.finite()) {
      return false;
    }
    if (s.kind == Kind::Tone) {
      if (y.high < 0 && s.signed_luminance) {
        y = -y;
      } else if (y.low < 0) {
        return false;
      }
      const Fast z = point(s.gain) * y;
      auto k = point(s.gain) / (point(1) + z);
      if (s.extended) {
        k = point(s.gain) * (point(1) + z / c.fast_white_squared) /
            (point(1) + z);
      }
      for (unsigned i = 0; i < count; ++i) {
        result[i] = k * x[i];
      }
    } else {
      if (y.high <= 0 || y.low >= 1) {
        output->fill(y.high <= 0 ? 0 : one(s.narrow));
        return true;
      }
      if (y.low <= 0 || y.high >= 1) {
        return false;
      }
      std::array<Fast, 3> d{};
      Fast q{};
      for (unsigned i = 0; i < 3; ++i) {
        d[i] = x[i] - y;
        Fast t;
        if (d[i].low >= 0) {
          t = d[i] / (point(1) - y);
        } else if (d[i].high < 0) {
          t = -d[i] / y;
        } else {
          return false;
        }
        q.low = std::max(q.low, t.low);
        q.high = std::max(q.high, t.high);
      }
      if (!q.finite()) {
        return false;
      }
      if (q.high <= s.knee) {
        *output = input;
        return true;
      }
      if (q.low <= s.knee) {
        return false;
      }
      const Fast u = q - point(s.knee), v = c.fast_knee_complement;
      const Fast scale = (point(s.knee) + v * u / (u + v)) / q;
      for (unsigned i = 0; i < 3; ++i) {
        result[i] = y + d[i] * scale;
      }
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    if (requested & (1U << i)) {
      auto bits = strict_fast(result[i], s.narrow);
      if (!bits) {
        return false;
      }
      (*output)[i] = *bits;
    }
  }
  return true;
#endif
}
Status rational(const State& s, RationalWork* work, const Bits& input,
                unsigned requested, Bits* output, const Math::Work& consume) {
  if (fast_rational(s, input, requested, output)) {
    return Status::success();
  }
#ifndef PHOTOSPIDER_FMT_MAPPING_REFERENCE
  if (s.kind == Kind::Display) {
    for (unsigned i = 0; i < s.selection.channels.size(); ++i) {
      if (requested & (1U << i)) {
        auto value = s.constants->affine->evaluate(
            {input[i], 0, one(s.narrow)}, s.narrow, &work->row, consume);
        if (!value.ok()) {
          return value.status();
        }
        (*output)[i] = value.value();
      }
    }
    return Status::success();
  }
#endif
  auto& math = work->math;
  math.reset();
  const auto& c = *s.constants;
  const auto count = static_cast<unsigned>(s.selection.channels.size());
  std::array<Rational, 3> x{};
  for (unsigned i = 0; i < count; ++i) {
    if (!s.componentwise || (requested & (1U << i))) {
      x[i] = math.binary(input[i], s.narrow);
    }
  }
  const auto zero = math.integer(0), unit = math.integer(1);
  if (s.kind == Kind::Display) {
    for (unsigned i = 0; i < count; ++i) {
      if (requested & (1U << i)) {
        auto value = s.to_nits
                         ? math.add(c.black, math.multiply(c.range, x[i]))
                         : math.divide(math.subtract(x[i], c.black), c.range);
        (*output)[i] = rounded(math, std::move(value), s.narrow);
      }
    }
  } else {
    auto y = count == 1 ? x[0] : zero;
    if (count == 3) {
      for (unsigned i = 0; i < 3; ++i) {
        y = math.add(y, math.multiply(c.weights[i], x[i]));
      }
    }
    if (s.kind == Kind::Tone) {
      const auto z = math.multiply(c.gain, math.absolute(y));
      auto k = math.divide(c.gain, math.add(unit, z));
      if (s.extended) {
        k = math.divide(
            math.multiply(c.gain,
                          math.add(unit, math.divide(z, c.white_squared))),
            math.add(unit, z));
      }
      for (unsigned i = 0; i < count; ++i) {
        if (requested & (1U << i)) {
          (*output)[i] = rounded(math, math.multiply(k, x[i]), s.narrow);
        }
      }
    } else {
      const int low = math.order(y, zero), high = math.order(y, unit);
      if (!math.active()) {
        return math.status();
      }
      if (low <= 0 || high >= 0) {
        output->fill(low <= 0 ? 0 : one(s.narrow));
      } else {
        std::array<Rational, 3> d{};
        auto q = zero;
        for (unsigned i = 0; i < 3; ++i) {
          d[i] = math.subtract(x[i], y);
          auto t = math.order(d[i], zero) >= 0
                       ? math.divide(d[i], math.subtract(unit, y))
                       : math.divide(math.negate(d[i]), y);
          if (math.order(t, q) > 0) {
            q = std::move(t);
          }
        }
        const int order = math.order(q, c.knee);
        if (!math.active()) {
          return math.status();
        }
        if (order <= 0) {
          *output = input;
          return Status::success();
        }
        const auto u = math.subtract(q, c.knee),
                   v = math.subtract(unit, c.knee);
        const auto scale = math.divide(
            math.add(c.knee, math.divide(math.multiply(v, u), math.add(u, v))),
            q);
        for (unsigned i = 0; i < 3; ++i) {
          if (requested & (1U << i)) {
            (*output)[i] = rounded(
                math, math.add(y, math.multiply(d[i], scale)), s.narrow);
          }
        }
      }
    }
  }
  if (!math.active()) {
    return math.status();
  }
  return Status::success();
}
Result<std::uint64_t> matrix_row(const State& s, ReductionWork* w,
                                 const basis_ops::ExactRow& row,
                                 const std::array<std::uint64_t, 3>& input,
                                 const Math::Work& consume) {
#ifndef PHOTOSPIDER_FMT_MAPPING_REFERENCE
  if (s.narrow && row.candidate_available) {
    for (unsigned i = 0; i < 3; ++i) {
      w->candidate.input[i][0] = numeric_ops::numeric_double(input[i], true);
    }
    basis_ops::candidates(&w->candidate, 1, row,
                          numeric_ops::SequenceProfile::Strict);
    std::uint64_t value;
    if (basis_ops::certify(row, w->candidate, 0, true,
                           numeric_ops::SequenceProfile::Strict, &value)) {
      return Result<std::uint64_t>(value);
    }
  }
#endif
  return row.evaluate(input, s.narrow, &w->row, consume);
}
Status reduce(const State& s, ReductionWork* w, const Bits& input, Bits* output,
              const Math::Work& consume) {
  if (in_cube(input, s.narrow)) {
    *output = input;
    return Status::success();
  }
  const auto& c = *s.constants;
  std::array<std::uint64_t, 3> xyz{}, lab{};
  for (unsigned i = 0; i < 3; ++i) {
    auto value =
        matrix_row(s, w, c.forward[i], {input[0], input[1], input[2]}, consume);
    if (!value.ok()) {
      return value.status();
    }
    xyz[i] = value.value();
  }
  for (unsigned i = 0; i < 3; ++i) {
    auto value = w->lab.evaluate(xyz, i);
    if (!value.ok()) {
      return value.status();
    }
    lab[i] = value.value();
  }
  const double light = numeric_ops::numeric_double(lab[0], s.narrow);
  if (light <= 0 || light >= 1) {
    output->fill(light <= 0 ? 0 : one(s.narrow));
    return Status::success();
  }
  auto& math = w->math;
  math.reset();
  const auto a = math.binary(lab[1], s.narrow),
             b = math.binary(lab[2], s.narrow);
  const auto candidate = [&](const Rational& t, Bits* result) -> Status {
    auto cancelled = consume(1);
    if (!cancelled.ok()) {
      return cancelled;
    }
    std::array<std::uint64_t, 3> scaled{
        lab[0], rounded(math, math.multiply(t, a), s.narrow),
        rounded(math, math.multiply(t, b), s.narrow)};
    if (!math.active()) {
      return math.status();
    }
    std::array<std::uint64_t, 3> coordinates{};
    for (unsigned i = 0; i < 3; ++i) {
      auto value = w->xyz.evaluate(scaled, i);
      if (!value.ok()) {
        return value.status();
      }
      coordinates[i] = value.value();
    }
    for (unsigned i = 0; i < 3; ++i) {
      auto value = matrix_row(s, w, c.inverse[i], coordinates, consume);
      if (!value.ok()) {
        return value.status();
      }
      (*result)[i] = value.value();
    }
    return Status::success();
  };
  auto lo = math.integer(0), hi = math.integer(1);
  auto status = candidate(lo, output);
  if (!status.ok()) {
    return status;
  }
  if (!in_cube(*output, s.narrow)) {
    for (unsigned i = 0; i < 3; ++i) {
      (*output)[i] = clipped((*output)[i], s.narrow);
    }
    return Status::success();
  }
  Bits trial{};
  status = candidate(hi, &trial);
  if (!status.ok()) {
    return status;
  }
  if (in_cube(trial, s.narrow)) {
    *output = trial;
    return Status::success();
  }
  for (unsigned step = 0; step < (s.narrow ? 32U : 64U); ++step) {
    auto middle = math.divide(math.add(lo, hi), math.integer(2));
    if (!math.active()) {
      return math.status();
    }
    status = candidate(middle, &trial);
    if (!status.ok()) {
      return status;
    }
    if (in_cube(trial, s.narrow)) {
      lo = std::move(middle);
      *output = trial;
    } else {
      hi = std::move(middle);
    }
  }
  return math.status();
}
void interpretation(State* s, const Parameters& p) {
  auto& interpretation = s->selection.interpretation;
  const bool tone = s->kind == Kind::Tone || s->kind == Kind::Display;
  if (!s->selection.semantic) {
    interpretation.model = text(p, "model", tone ? "" : "rgb");
    if (tone) {
      require(p.count("model") && p.count("reference") && p.count("units"),
              "raw luminance mapping requires model/reference/units");
      interpretation.reference = text(p, "reference");
    }
    interpretation.transfer = "linear";
  }
  typed(
      interpretation.model == "rgb" || (tone && interpretation.model == "gray"),
      "mapping requires RGB or linear-Y Gray");
  const bool gray = interpretation.model == "gray";
  typed(s->selection.channels.size() == (gray ? 1U : 3U),
        "wrong color component count");
  require(interpretation.transfer == "linear",
          "mapping requires linear coordinates");
  if (gray && s->selection.semantic) {
    require(interpretation.coordinates &&
                interpretation.coordinates->gray_kind == "linear_y",
            "Gray must use linear_y");
  }
  if (interpretation.reference == "scene_relative" ||
      interpretation.reference == "display_relative" ||
      interpretation.reference == "display_absolute" ||
      interpretation.reference == "absolute_display_cd_m2") {
    const bool source_absolute =
        interpretation.reference == "display_absolute" ||
        interpretation.reference == "absolute_display_cd_m2";
    if (!interpretation.coordinates) {
      interpretation.coordinates.emplace();
    }
    require(interpretation.coordinates->scale.empty() ||
                interpretation.coordinates->scale ==
                    (source_absolute ? "absolute" : "relative"),
            "reference and explicit scale disagree");
    interpretation.coordinates->scale =
        source_absolute ? "absolute" : "relative";
    interpretation.reference =
        interpretation.reference == "scene_relative" ? "scene" : "display";
  }
  const bool absolute = s->kind == Kind::Display && !s->to_nits;
  if (s->selection.semantic) {
    require(interpretation.reference == "scene" ||
                interpretation.reference == "display",
            "explicit native reference required");
    for (const auto& unit : s->selection.units) {
      require(absolute ? (unit == "cd/m2" || unit == "cd/m^2")
                       : (unit == "relative" || unit == "1" ||
                          (gray && unit == "relative_luminance")),
              "mapping coordinate units disagree");
    }
    if (interpretation.coordinates &&
        !interpretation.coordinates->scale.empty()) {
      require(interpretation.coordinates->scale ==
                  (absolute ? "absolute" : "relative"),
              "mapping scale disagrees");
    }
  } else if (tone) {
    const auto unit = text(p, "units");
    require(absolute ? (unit == "cd/m2" || unit == "cd/m^2")
                     : (unit == "relative" || unit == "1" ||
                        (gray && unit == "relative_luminance")),
            "raw units disagree");
  }
  if (s->kind == Kind::Display) {
    require(interpretation.reference == "display",
            "display mapping reference disagrees");
  } else if (tone || s->selection.semantic) {
    require(interpretation.reference == "scene" ||
                interpretation.reference == "display",
            "relative reference required");
  }
}
OperationPreparation prepare_impl(const std::vector<OperationMetadata>& inputs,
                                  const Parameters& p, Kind kind) {
  FloatingEnvironment environment;
  typed(environment.active(), "floating environment unavailable");
  require(inputs.size() == 1, "one color input required");
  require(
      text(p, "layout", "auto") == "auto" || text(p, "layout") == "materialize",
      "mapping layout must be auto or materialize");
  auto s = std::make_shared<State>();
  s->kind = kind;
  s->symbol = "native-color-mapping";
  s->contract = "native-color-mapping-unchecked-v2";
  const bool tone = kind == Kind::Tone || kind == Kind::Display;
  // Select model before assigning the RGB role order.
  s->selection = select(inputs[0], p);
  const bool rgb = !tone || (s->selection.semantic
                                 ? s->selection.interpretation.model == "rgb"
                                 : text(p, "model") == "rgb");
  s->selection = rgb ? select(inputs[0], p, {"red", "green", "blue"})
                     : select(inputs[0], p, {"gray"});
  require(rgb || !p.count("target_basis"),
          "Gray mapping does not take an RGB basis");
  if (s->selection.semantic) {
    require(!p.count("model") && !p.count("reference") && !p.count("units"),
            "described mapping forbids raw model/reference/units");
  }
  if (kind == Kind::Display) {
    const auto direction = text(p, "direction");
    require(direction == "relative_to_nits" || direction == "nits_to_relative",
            "invalid display direction");
    s->to_nits = direction == "relative_to_nits";
    require(p.count("black_nits") && p.count("white_nits"),
            "display range required");
    s->black = real(p, "black_nits", 0);
    s->white_nits = real(p, "white_nits", 1);
  }
  interpretation(s.get(), p);
  if (kind == Kind::Check) {
    mask_shape(s.get(), inputs[0], boolean(p, "keepdims"));
  } else {
    same_shape(s.get(), inputs[0]);
  }
  s->componentwise = kind == Kind::Clip || kind == Kind::Display;
  auto c = std::make_shared<Constants>();
  Math math(numeric_ops::SequenceProfile::Strict);
  const auto no_work = [](std::uint64_t) { return Status::success(); };
  math.bind(no_work);
  if (kind == Kind::Tone) {
    const auto curve = text(p, "curve", "simple");
    require(curve == "simple" || curve == "white_extended",
            "unknown luminance curve");
    s->extended = curve == "white_extended";
    require(s->extended == (p.count("white") != 0),
            "white is required only for white_extended");
    s->gain = real(p, "gain", 1);
    s->white = real(p, "white", 1);
    const auto negative = text(p, "negative_luminance", "reject");
    require(negative == "reject" || negative == "signed_extension",
            "unknown negative luminance policy");
    s->signed_luminance = negative == "signed_extension";
  }
  if (kind == Kind::Compress) {
    s->knee = real(p, "knee", .8);
  }
  c->gain = math.binary(basis_ops::bits(s->gain));
  const auto white = math.binary(basis_ops::bits(s->white));
  c->white_squared = math.multiply(white, white);
  c->knee = math.binary(basis_ops::bits(s->knee));
  c->black = math.binary(basis_ops::bits(s->black));
  c->range =
      math.subtract(math.binary(basis_ops::bits(s->white_nits)), c->black);
  checked(math.status());
  c->fast_white_squared = Fast::point(s->white) * Fast::point(s->white);
  c->fast_range = Fast::point(s->white_nits) - Fast::point(s->black);
  c->fast_knee_complement = Fast::point(1) - Fast::point(s->knee);
  if (kind == Kind::Display) {
    const auto black = basis_ops::number(s->black);
    const auto range =
        basis_ops::subtract(basis_ops::number(s->white_nits), black);
    basis_ops::Matrix coefficients{};
    coefficients[0] =
        s->to_nits ? range : basis_ops::divide(basis_ops::number(1), range);
    coefficients[1] = basis_ops::number(0);
    coefficients[2] =
        s->to_nits
            ? black
            : basis_ops::divide(
                  basis_ops::subtract(basis_ops::number(0), black), range);
    c->affine.emplace(coefficients, 0);
  }
  const bool basis_needed = kind == Kind::Reduce || kind == Kind::Compress ||
                            (kind == Kind::Tone && rgb);
  if (basis_needed || p.count("target_basis") ||
      (rgb && s->selection.semantic)) {
    const auto& i = s->selection.interpretation;
    format::RgbBasis assertion;
    assertion.preset = i.primaries;
    assertion.primaries_xy = i.primaries_xy;
    assertion.white = i.white;
    auto basis = p.count("target_basis")
                     ? basis_ops::parse_basis(text(p, "target_basis"))
                     : basis_ops::resolve(assertion);
    require(
        i.primaries.empty() || basis_ops::preset(i.primaries).xy == basis.xy,
        "basis assertion disagrees");
    require(!i.primaries_xy || *i.primaries_xy == basis.xy,
            "primary assertion disagrees");
    require(!i.white || *i.white == basis.white, "white assertion disagrees");
    const auto matrix = basis_ops::rgb_matrix(basis.xy, basis.white);
    for (unsigned row = 0; row < 3; ++row) {
      c->forward.emplace_back(matrix, row);
    }
    for (unsigned i = 0; i < 3; ++i) {
      c->weights[i] = imported(matrix[3 + i]);
      const auto nearest = math.rounded(c->weights[i], false);
      checked(math.status());
      const auto value = numeric_ops::numeric_double(nearest);
      c->fast_weights[i] = {numeric_ops::numeric_down(value),
                            numeric_ops::numeric_up(value)};
    }
    if (kind == Kind::Reduce) {
      require(basis.white == std::array<double, 2>{.3127, .3290},
              "OKLab gamut mapping requires D65");
      const auto inverse = basis_ops::inverse(matrix);
      for (unsigned row = 0; row < 3; ++row) {
        c->inverse.emplace_back(inverse, row);
      }
      c->lab_config.kind = model_ops::Kind::XyzToOklab;
      c->lab_config.narrow = s->narrow;
      c->xyz_config = c->lab_config;
      c->xyz_config.kind = model_ops::Kind::OklabToXyz;
      c->lab = checked(model_ops::prepare_constants(c->lab_config));
      c->xyz = checked(model_ops::prepare_constants(c->xyz_config));
    }
  }
  checked(math.status());
  s->constants = std::move(c);
  s->scratch = kind == Kind::Reduce ? sizeof(ReductionWork)
               : kind == Kind::Clip || kind == Kind::Check
                   ? 0
                   : sizeof(RationalWork);
  OperationOutputSpecialization output;
  output.metadata.result_schema = s->output_schema;
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(s);
  return result;
}
struct Prepared final {
  State state;
  fixed_tensor::PointInputs<1> inputs;
  unsigned requested = 0;
  bool arithmetic = false;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& prepared =
      *static_cast<const Prepared*>(invocation->prepared_context);
  const auto& s = prepared.state;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& value) {
    if (!value.ok()) {
      const char* message = value.message.c_str();
      if (value.message.empty() &&
          value.reason == FailureReason::InvalidDomain) {
        message = "invalid color domain";
      }
      if (value.message.empty() &&
          value.reason == FailureReason::ArithmeticOverflow) {
        message = "color arithmetic overflow";
      }
      *status = kernel_failure(value.code, message, value.reason);
    }
    return value.ok();
  };
  if (!accept(consume(1))) {
    return;
  }
  FloatingEnvironment environment;
  if (prepared.arithmetic && !environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "color floating environment unavailable");
    return;
  }
  auto destroy_rational = [](RationalWork* w) {
    if (w) {
      w->~RationalWork();
    }
  };
  auto destroy_reduce = [](ReductionWork* w) {
    if (w) {
      w->~ReductionWork();
    }
  };
  std::unique_ptr<RationalWork, decltype(destroy_rational)> rational_work(
      prepared.arithmetic && s.scratch && s.kind != Kind::Reduce
          ? new (invocation->scratch[0].allocation) RationalWork(consume)
          : nullptr,
      destroy_rational);
  std::unique_ptr<ReductionWork, decltype(destroy_reduce)> reduction_work(
      prepared.arithmetic && s.kind == Kind::Reduce
          ? new (invocation->scratch[0].allocation)
                ReductionWork(*s.constants, consume)
          : nullptr,
      destroy_reduce);
  fixed_tensor::PointReader<1> reader{prepared.inputs, *invocation};
  const auto& out = invocation->outputs[0];
  const auto& layout = *out.layout;
  const unsigned width = s.narrow ? 4 : 8;
  auto* destination = static_cast<std::uint8_t*>(out.allocation);
  std::uint64_t points = 1;
  for (unsigned a = 0; a < layout.rank; ++a) {
    if (static_cast<int>(a) != s.output_axis) {
      points *= layout.shape[a];
    }
  }
  std::uint64_t writes = 0;
  for (std::uint64_t point = 0; point < points; ++point) {
    if ((point % 64 == 0) && !accept(consume(64))) {
      return;
    }
    auto index = point;
    std::array<std::uint64_t, 8> at{};
    for (unsigned a = layout.rank; a-- > 0;) {
      if (static_cast<int>(a) != s.output_axis) {
        at[a] = layout.origin[a] + index % layout.shape[a];
        index /= layout.shape[a];
      } else {
        at[a] = layout.origin[a];
      }
    }
    auto source = source_position(s, at, layout.rank);
    Bits input{}, result{};
    if (prepared.arithmetic) {
      for (unsigned i = 0; i < s.selection.channels.size(); ++i) {
        if (!s.componentwise || (prepared.requested & (1U << i))) {
          if (s.selection.axis >= 0) {
            source[s.selection.axis] = s.selection.channels[i];
          }
          input[i] = reader.bits(0, source, width);
        }
      }
      Status evaluated;
      if (s.kind == Kind::Clip) {
        for (unsigned i = 0; i < 3; ++i) {
          if (prepared.requested & (1U << i)) {
            result[i] = clipped(input[i], s.narrow);
          }
        }
      } else if (s.kind == Kind::Check) {
        result[0] = in_cube(input, s.narrow) ? 0 : one(s.narrow);
      } else if (s.kind == Kind::Reduce) {
        evaluated = reduce(s, reduction_work.get(), input, &result, consume);
      } else {
        evaluated = rational(s, rational_work.get(), input, prepared.requested,
                             &result, consume);
      }
      if (!accept(evaluated)) {
        return;
      }
    }
    for (const auto& span : s.spans) {
      auto first = span.first, end = span.first + span.count;
      if (s.output_axis >= 0) {
        first = std::max(first, layout.origin[s.output_axis]);
        end = std::min(
            end, layout.origin[s.output_axis] + layout.shape[s.output_axis]);
      }
      for (auto channel = first; channel < end; ++channel) {
        if ((writes++ % 256 == 0) && !accept(consume(256))) {
          return;
        }
        if (s.output_axis >= 0) {
          at[s.output_axis] = channel;
        }
        std::uint64_t bits = 0;
        if (span.component >= 0) {
          bits = result[span.component];
        } else {
          source[s.selection.axis] = span.source + channel - span.first;
          bits = reader.bits(0, source, width);
        }
        std::memcpy(destination + offset(out, at), &bits, width);
      }
    }
  }
  *status = {};
}
OperationDefinition definition(Kind kind, const char* key) {
  OperationDefinition op;
  op.key = key;
  op.traits = traits(kind == Kind::Check ? "mask" : "values");
  auto& parameters = op.traits.parameter_schema;
  if (kind == Kind::Reduce || kind == Kind::Compress || kind == Kind::Tone) {
    parameters.push_back(
        {"target_basis", OperationParameterType::String, false});
  }
  if (kind == Kind::Check) {
    parameters.push_back({"keepdims", OperationParameterType::Bool, false});
  }
  if (kind == Kind::Compress) {
    parameters.push_back({"knee", OperationParameterType::Float64, false});
  }
  if (kind == Kind::Tone || kind == Kind::Display) {
    for (auto field : {"model", "reference", "units"}) {
      parameters.push_back({field, OperationParameterType::String, false});
    }
  }
  if (kind == Kind::Tone) {
    for (auto field : {"curve", "negative_luminance"}) {
      parameters.push_back({field, OperationParameterType::String, false});
    }
    for (auto field : {"gain", "white"}) {
      parameters.push_back({field, OperationParameterType::Float64, false});
    }
  }
  if (kind == Kind::Display) {
    parameters.push_back({"direction", OperationParameterType::String, true});
    parameters.push_back({"black_nits", OperationParameterType::Float64, true});
    parameters.push_back({"white_nits", OperationParameterType::Float64, true});
  }
  op.prepare_static = [kind](const auto& inputs,
                             const auto& p) -> Result<OperationPreparation> {
    try {
      return Result<OperationPreparation>(prepare_impl(inputs, p, kind));
    } catch (const basis_ops::CheckedFailure& f) {
      return Result<OperationPreparation>(f.status);
    } catch (const basis_ops::GeometryError&) {
      return Result<OperationPreparation>(
          basis_ops::invalid("invalid mapping geometry"));
    } catch (const data_internal::format_numeric::ExactWorkFailure& f) {
      return Result<OperationPreparation>(f.status);
    }
  };
  op.lower = lower<State>;
  op.prepare_kernel =
      [](const physical::KernelDesc& kernel, const physical::Placement& place,
         const KernelPreparation& q, const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto* s = static_cast<const State*>(q.static_state);
    if (!s || kernel.symbol != s->symbol || kernel.contract != s->contract) {
      return Answer(Status{ErrorCode::TypeMismatch, "invalid mapping kernel"});
    }
    if (place.backend != physical::Backend::Cpu ||
        place.domain != physical::Domain::Host) {
      return Answer(
          Status{ErrorCode::BackendUnavailable, "mapping requires CPU Host"});
    }
    auto prepared = std::make_unique<Prepared>();
    prepared->state = *s;
    auto valid = prepare_inputs(*s, q, &prepared->inputs);
    if (!valid.ok()) {
      return Answer(valid);
    }
    const auto region = fixed_tensor::view_region(q.writes[0].layout);
    prepared->arithmetic = calculates(*s, region);
    for (const auto& span : s->spans) {
      if (span.component >= 0 && intersects(*s, span, region)) {
        prepared->requested |= 1U << span.component;
      }
    }
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = prepared.release();
    result.invoke_cpu = invoke;
    result.release = [](const void* ptr) {
      delete static_cast<const Prepared*>(ptr);
    };
    return Answer(result);
  };
  return op;
}
}  // namespace ps::plugin_internal::mapping_ops

namespace ps::plugin_internal {
Status register_gamut_tone(OperationRegistry* registry) {
  using mapping_ops::Kind;
  const std::array<std::pair<Kind, const char*>, 6> members{
      {{Kind::Clip, "color.clip_rgb_gamut_strict"},
       {Kind::Reduce, "color.reduce_oklab_chroma_v1_strict"},
       {Kind::Compress, "color.compress_rgb_chroma_v1_strict"},
       {Kind::Check, "color.check_rgb_gamut_strict"},
       {Kind::Tone, "color.map_luminance_v1_strict"},
       {Kind::Display, "color.convert_display_luminance_strict"}}};
  for (const auto& member : members) {
    auto result = registry->register_operation(
        mapping_ops::definition(member.first, member.second));
    if (!result.ok()) {
      return result;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
