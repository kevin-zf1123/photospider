#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/directed_color_functions.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "02-format-color/model_rational.hpp"

namespace ps::plugin_internal::model_ops {
enum class Kind : unsigned {
  XyzToLab,
  LabToXyz,
  LabToLch,
  LchToLab,
  XyzToOklab,
  OklabToXyz,
  OklabToOklch,
  OklchToOklab,
  RgbToHsl,
  HslToRgb,
  RgbToHsv,
  HsvToRgb,
  RgbToYcbcr,
  YcbcrToRgb,
  XyzToXyy,
  XyyToXyz,
  ColorToGray,
  GrayToColor,
  Threshold,
  BinaryToGray
};
enum class GrayKind { LinearY, EncodedLuma, CielabL, OklabL };
enum class Algorithm { Auto, Scalar, Reference };
struct MathConfig final {
  Kind kind = Kind::XyzToLab;
  GrayKind gray = GrayKind::LinearY;
  numeric_ops::SequenceProfile profile = numeric_ops::SequenceProfile::Strict;
  Algorithm algorithm = Algorithm::Auto;
  bool narrow = false, semantic = true, input_pi = false, output_pi = false;
  std::array<double, 2> white{.3127, .3290};
  std::array<double, 2> ncl{.2126, .0722};
  double threshold = 0;
  std::array<std::uint64_t, 2> levels{};
};
using Rational = RationalMath::Rational;
using Matrix = std::array<std::array<Rational, 3>, 3>;
using Fast = numeric_ops::FastInterval;
struct Constants final {
  Matrix first, second;
  std::array<std::array<Fast, 3>, 3> first_fast{}, second_fast{};
  std::array<Rational, 2> white_xy, white_xz;
  std::array<Fast, 2> white_fast{}, white_xz_fast{};
  Rational epsilon, delta;
  Fast epsilon_fast{}, delta_fast{};
};
Result<std::shared_ptr<const Constants>> prepare_constants(
    const MathConfig& config);
unsigned support(Kind kind, unsigned component, GrayKind gray);
// -1 denotes arithmetic, -2 a source-independent canonical +0.
int copied_component(Kind kind, unsigned component, GrayKind gray);
const char* operation_name(Kind kind);
const char* source_model(Kind kind, GrayKind gray);
const char* target_model(Kind kind, GrayKind gray);

class ModelMath final {
  const MathConfig& config_;
  const Constants& constants_;
  RationalMath exact_;
  struct IntervalWork final {
    RationalMath* exact;
    Status operator()(std::uint64_t amount) const {
      exact->work(amount);
      return exact->status();
    }
  } interval_work_{&exact_};
  using Math = numeric_ops::CheckedDirectedInterval;
  numeric_ops::DirectedColorFunctionsStorage<Math> directed_;
  using Interval = Math::Interval;
  using Frame = Math::Frame;
  bool active();
  bool refine(unsigned precision);
  std::uint64_t angle(std::uint64_t y, std::uint64_t x);
  struct RootTerm final {
    Rational argument;
    std::optional<Rational> exact;
  };
  Rational dot(const std::array<Rational, 3>& row,
               const std::array<Rational, 3>& input, unsigned mask = 7);
  RootTerm cie_f(const Rational& value);
  Rational cie_g(const Rational& value);
  RootTerm root(const Rational& value);
  void enclose_root(Interval result, const RootTerm& value);
  std::uint64_t root_sum(const std::array<RootTerm, 3>& roots,
                         const std::array<Rational, 3>& weights,
                         const Rational& bias);
  std::uint64_t times_pi(const Rational& value);
  std::uint64_t inverse_polar(const std::array<Rational, 3>& input,
                              const std::array<std::uint64_t, 3>& bits,
                              unsigned component);
  std::uint64_t inverse_hue(const std::array<Rational, 3>& input,
                            const std::array<std::uint64_t, 3>& bits,
                            unsigned component);
  unsigned radian_sector(Interval q, std::uint64_t hue);
  std::uint64_t reference(const std::array<std::uint64_t, 3>& bits,
                          unsigned component);
  std::optional<Fast> fast(const std::array<std::uint64_t, 3>& bits,
                           unsigned component);
  std::optional<std::uint64_t> accept(
      Fast bound, std::optional<double> candidate = {}) const;

 public:
  ModelMath(const MathConfig& config, const Constants& constants,
            RationalMath::Work work);
  ModelMath(const ModelMath&) = delete;
  ModelMath& operator=(const ModelMath&) = delete;
  ModelMath(ModelMath&&) = delete;
  ModelMath& operator=(ModelMath&&) = delete;
  Result<std::uint64_t> evaluate(const std::array<std::uint64_t, 3>& bits,
                                 unsigned component,
                                 std::optional<double> candidate = {});
};
}  // namespace ps::plugin_internal::model_ops
