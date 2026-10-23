#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/accelerated_math.hpp"
#include "03-generation/fast_math.hpp"
#include "03-generation/noise_math.hpp"
#include "03-generation/perlin_dyadic.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
using CompactExact = model_ops::RationalMathStorage<32>;
struct PolynomialScratch {
  Exact wide{numeric_ops::SequenceProfile::Strict};
  CompactExact compact{numeric_ops::SequenceProfile::Strict};
};
// Reduce the integer phase before multiplying by pi. The selected SLEEF
// sin/cos argument is in [0,pi/4]; its four-ULP enclosure is enlarged by the
// complete argument interval using |sin'|,|cos'| <= 1. A successful result
// proves the same final RN32 as the full Box-Muller expression.
std::optional<std::uint64_t> grain_white_fast(double u, std::uint64_t phase) {
  if (!numeric_ops::accelerated_math_available())
    return {};
  double logarithm = 0;
  photospider_sleef_evaluate(1, &u, &u, &logarithm, 1);
  const auto radius = fast_sqrt(
      -numeric_ops::accelerated_math_enclosure(logarithm) * Fast::point(2));
  const unsigned quadrant = static_cast<unsigned>(phase >> 51);
  auto remainder = phase & ((UINT64_C(1) << 51) - 1);
  bool sine = quadrant & 1;
  if (remainder > (UINT64_C(1) << 50)) {
    remainder = (UINT64_C(1) << 51) - remainder;
    sine = !sine;
  }
  const Fast pi{0x1.921fb54442d18p+1, 0x1.921fb54442d19p+1};
  const auto angle = pi * Fast::point(static_cast<double>(remainder) * 0x1p-52);
  double trigonometric = 0;
  photospider_sleef_evaluate(sine ? 2 : 3, &angle.low, &angle.low,
                             &trigonometric, 1);
  const double error = numeric_ops::numeric_up(angle.high - angle.low);
  auto cosine = numeric_ops::accelerated_math_enclosure(trigonometric) +
                Fast{-error, error};
  if (quadrant == 1 || quadrant == 2)
    cosine = -cosine;
  return fast_result(radius * cosine, true, false);
}
enum class NoiseKind { Uniform, Gaussian, Perlin, Gradient };
struct Noise final : Program {
  NoiseKind kind;
  bool grain_white = false;
  ElementType type = ElementType::Float64;
  std::uint64_t height = 1, width = 1, channels = 1;
  std::int64_t origin_x = 0, origin_y = 0;
  std::uint64_t seed = 0, stream = 0, frame = 0;
  std::array<std::int64_t, 2> period{};
  unsigned dimensions = 0;
  explicit Noise(NoiseKind selected, const Parameters& p)
      : kind(selected), type(dtype(p)) {
    seed = static_cast<std::uint64_t>(integer(p, "seed"));
    stream = static_cast<std::uint64_t>(integer(p, "stream"));
    frame = static_cast<std::uint64_t>(integer(p, "frame"));
    origin_x = integer(p, "origin_x");
    origin_y = integer(p, "origin_y");
    period = {integer(p, "period_x"), integer(p, "period_y")};
    scratch_bytes = kind == NoiseKind::Uniform    ? 0
                    : kind == NoiseKind::Gaussian ? sizeof(Math)
                    : kind == NoiseKind::Perlin   ? sizeof(PerlinScratch)
                                                  : sizeof(PolynomialScratch);
  }
  std::vector<ReadRegion> support(unsigned out,
                                  const Region& region) const override {
    if (grain_white)
      return {};
    if (kind == NoiseKind::Uniform || whole_inputs) {
      return Program::support(out, region);
    }
    if (kind == NoiseKind::Gaussian) {
      Region selected({region.dimensions()[2]});
      return {{0, 0, selected}, {1, 0, selected}};
    }
    auto dimensions = region.dimensions();
    dimensions.push_back({0, this->dimensions});
    return {{0, 0, Region(std::move(dimensions))}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    const bool narrow = type == ElementType::Float32;
    const auto& out = invocation.outputs[0];
    if (kind == NoiseKind::Uniform) {
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll(10);
        const auto word = philox(origin_x + static_cast<std::int64_t>(at[1]),
                                 origin_y + static_cast<std::int64_t>(at[0]),
                                 at[2], 1, seed, stream, frame)[0];
        store(out, at,
              convert(narrow ? static_cast<double>(word >> 40) * 0x1p-24
                             : static_cast<double>(word >> 11) * 0x1p-53,
                      narrow));
      });
      return;
    }
    if (kind == NoiseKind::Gaussian) {
      MathArena arena(invocation.scratch[0].allocation, work);
      auto& m = *arena;
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        m.exact.reset();
        const auto mean = grain_white ? m.exact.integer(0)
                                      : m.input(reader, 0, 0, {at[2]}),
                   sigma = grain_white ? m.exact.integer(1)
                                       : m.input(reader, 1, 0, {at[2]});
        if (Exact::zero(sigma)) {
          store(out, at, m.rounded(mean, narrow, true));
          return;
        }
        const auto block =
            philox(origin_x + static_cast<std::int64_t>(at[1]),
                   origin_y + static_cast<std::int64_t>(at[0]), at[2],
                   grain_white ? 10 : 2, seed, stream, frame);
        const double u =
            static_cast<double>(2 * (block[0] >> 12) + 1) * 0x1p-53;
        const auto phase = block[1] >> 11;
        // Exact quarter turns avoid an interval that forever straddles zero.
        if (phase == (UINT64_C(1) << 51) || phase == (UINT64_C(3) << 51)) {
          store(out, at, m.rounded(mean, narrow));
          return;
        }
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
        if (grain_white && narrow) {
          work.poll(128);
          if (const auto result = grain_white_fast(u, phase)) {
            store(out, at, *result);
            return;
          }
        }
#endif
        const double v = static_cast<double>(phase) * 0x1p-53;
        const auto result = m.certify(narrow, [&](Interval value) {
          auto logarithm = m.d.interval(), radius = m.d.interval(),
               angle = m.d.interval(), sine = m.d.interval(),
               cosine = m.d.interval(), scale = m.d.interval();
          m.functions.functions.logarithm(
              logarithm, numeric_ops::BinaryParts::decode(bits(u), false));
          m.d.negate(logarithm, logarithm);
          m.d.scale(logarithm, logarithm, 1);
          m.sqrt(radius, logarithm);
          if (phase == 0 || phase == (UINT64_C(1) << 52)) {
            m.d.integer(cosine, phase ? -1 : 1);
          } else {
            m.d.constant(angle, true);
            m.d.raw(scale, bits(v), false);
            m.d.multiply(angle, angle, scale);
            m.d.scale(angle, angle, 1);
            m.sincos(sine, cosine, angle);
          }
          m.d.multiply(radius, radius, cosine);
          m.enclose(scale, sigma);
          m.d.multiply(radius, radius, scale);
          m.enclose(scale, mean);
          m.d.add(value, scale, radius);
        });
        store(out, at, result);
      });
      return;
    }
    if (kind == NoiseKind::Perlin) {
      auto* arithmetic =
          new (invocation.scratch[0].allocation) PerlinScratch(work);
      struct Destroy {
        PerlinScratch* p;
        ~Destroy() { p->~PerlinScratch(); }
      } destroy{arithmetic};
      const bool input_small = kernel.groups[0].type == ElementType::Float32;
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        std::array<std::uint64_t, 3> raw{};
        bool compact = true;
        for (unsigned axis = 0; axis < 3; ++axis) {
          auto coordinate = at;
          coordinate[out.layout->rank] = axis;
          raw[axis] = reader.bits(0, 0, coordinate);
          const auto parts =
              numeric_ops::BinaryParts::decode(raw[axis], input_small);
          compact &= !parts.significand || parts.exponent >= -149;
        }
        store(out, at,
              compact ? arithmetic->compact.evaluate(raw, input_small, narrow)
                      : arithmetic->wide.evaluate(raw, input_small, narrow));
      });
      return;
    }
    auto* arithmetic = new (invocation.scratch[0].allocation) PolynomialScratch;
    struct Destroy {
      PolynomialScratch* p;
      ~Destroy() { p->~PolynomialScratch(); }
    } destroy{arithmetic};
    arithmetic->wide.bind(work);
    arithmetic->compact.bind(work);
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      std::array<double, 3> q{};
      bool compact = true;
      for (unsigned a = 0; a < dimensions; ++a) {
        auto coordinate = at;
        coordinate[out.layout->rank] = a;
        q[a] = reader.number(0, 0, coordinate);
        const auto parts = numeric_ops::BinaryParts::decode(bits(q[a]), false);
        compact &= !parts.significand || parts.exponent >= -100;
      }
      std::uint64_t result;
      const auto evaluate = [&](auto& math) {
        math.reset();
        return gradient_exact(math, q, narrow, seed, stream, frame, period);
      };
      result =
          compact ? evaluate(arithmetic->compact) : evaluate(arithmetic->wide);
      store(out, at, result);
    });
  }
};
enum class Boundary { Zero, Clamp, Wrap, Reflect };
std::int64_t euclidean(std::int64_t value, std::int64_t period) {
  value %= period;
  return value < 0 ? value + period : value;
}
std::int64_t mapped(std::int64_t value, std::int64_t extent, Boundary mode) {
  if (value >= 0 && value < extent) {
    return value;
  }
  if (mode == Boundary::Zero) {
    return -1;
  }
  if (mode == Boundary::Clamp) {
    return std::max(INT64_C(0), std::min(extent - 1, value));
  }
  if (mode == Boundary::Wrap) {
    return euclidean(value, extent);
  }
  if (extent == 1) {
    return 0;
  }
  const auto remainder = euclidean(value, 2 * (extent - 1));
  return remainder < extent ? remainder : 2 * (extent - 1) - remainder;
}
std::vector<RegionDimension> mapped_support(RegionDimension query,
                                            std::uint64_t n,
                                            std::uint64_t radius,
                                            Boundary mode) {
  const auto first = static_cast<std::int64_t>(query.offset) -
                     static_cast<std::int64_t>(radius);
  const auto last =
      static_cast<std::int64_t>(query.offset + query.extent - 1 + radius);
  const auto extent = static_cast<std::int64_t>(n);
  if (mode == Boundary::Zero || mode == Boundary::Clamp) {
    const auto low = std::max(INT64_C(0), first),
               high = std::min(extent - 1, last);
    return low <= high
               ? std::vector<RegionDimension>{{static_cast<std::uint64_t>(low),
                                               static_cast<std::uint64_t>(
                                                   high - low + 1)}}
               : std::vector<RegionDimension>{};
  }
  if (mode == Boundary::Wrap) {
    if (last - first + 1 >= extent) {
      return {{0, n}};
    }
    const auto low = euclidean(first, extent), high = euclidean(last, extent);
    if (low <= high) {
      return {{static_cast<std::uint64_t>(low),
               static_cast<std::uint64_t>(high - low + 1)}};
    }
    return {{0, static_cast<std::uint64_t>(high + 1)},
            {static_cast<std::uint64_t>(low), n - low}};
  }
  if (n == 1 || last - first + 1 >= 2 * (extent - 1)) {
    return {{0, n}};
  }
  auto low = std::min(mapped(first, extent, mode), mapped(last, extent, mode));
  auto high = std::max(mapped(first, extent, mode), mapped(last, extent, mode));
  const auto period = 2 * (extent - 1);
  const auto until_min = euclidean(-first, period);
  const auto until_max = euclidean(extent - 1 - first, period);
  if (until_min <= last - first) {
    low = 0;
  }
  if (until_max <= last - first) {
    high = extent - 1;
  }
  return {{static_cast<std::uint64_t>(low),
           static_cast<std::uint64_t>(high - low + 1)}};
}
// Products have at most 106 significant bits. Accumulate positive and negative
// digits separately; carry once after the dot, not once per tap. With at most
// 2^40 taps, a bin is below 2^104 and fits unsigned 128-bit arithmetic.
struct ProductBins final {
  std::array<unsigned __int128, 68> positive{}, negative{};
  void reset() {
    positive.fill(0);
    negative.fill(0);
  }
  void add(const numeric_ops::BinaryParts& a,
           const numeric_ops::BinaryParts& b) {
    if (!a.significand || !b.significand) {
      return;
    }
    const auto product =
        static_cast<unsigned __int128>(a.significand) * b.significand;
    const unsigned shift =
        static_cast<unsigned>(a.exponent + b.exponent + 2148);
    const unsigned index = shift / 64, tail = shift % 64;
    const auto low = static_cast<std::uint64_t>(product),
               high = static_cast<std::uint64_t>(product >> 64);
    auto& bins = a.negative != b.negative ? negative : positive;
    bins[index] += low << tail;
    bins[index + 1] += tail ? (high << tail) | (low >> (64 - tail)) : high;
    if (tail) {
      bins[index + 2] += high >> (64 - tail);
    }
  }
  void finish(numeric_ops::RatioWorkspace* output) const {
    unsigned __int128 positive_carry = 0, negative_carry = 0;
    for (unsigned i = 0; i < positive.size(); ++i) {
      const auto p = positive[i] + positive_carry,
                 n = negative[i] + negative_carry;
      output->numerator.words[i] = static_cast<std::uint64_t>(p);
      output->term.words[i] = static_cast<std::uint64_t>(n);
      positive_carry = p >> 64;
      negative_carry = n >> 64;
    }
    output->negative = false;
    output->add_term(true);
  }
};
struct CorrelationScratch final {
  Math math;
  ProductBins bins;
  numeric_ops::RatioWorkspace dot{numeric_ops::SequenceProfile::Strict};
  numeric_ops::RatioWorkspace norm{numeric_ops::SequenceProfile::Strict};
  explicit CorrelationScratch(const Work& work) : math(work) {}
};
struct Correlation final : Program {
  ElementType type;
  Boundary boundary;
  unsigned normalization;
  std::uint64_t height, width, kh, kw;
  explicit Correlation(const std::vector<OperationMetadata>& inputs,
                       const Parameters& p)
      : type(dtype(p)) {
    const auto source = tensor(inputs[0]).sample_shape(),
               kernel = tensor(inputs[1]).sample_shape();
    require(
        source.size() == 3 && kernel.size() == 2 &&
            floating(tensor(inputs[0]).descriptor.element_type) &&
            floating(tensor(inputs[1]).descriptor.element_type),
        "correlation requires floating HWC source and two-dimensional kernel");
    const auto configured_limit = integer(p, "max_kernel_extent", 255);
    require(configured_limit > 0,
            "correlation kernel capacity must be positive");
    const auto limit = static_cast<std::uint64_t>(configured_limit);
    require((kernel[0] & 1) && (kernel[1] & 1) && kernel[0] <= limit &&
                kernel[1] <= limit,
            "correlation kernel extents must be admitted odd sizes");
    height = source[0];
    width = source[1];
    kh = kernel[0];
    kw = kernel[1];
    const auto mode = text(p, "boundary");
    require(mode == "zero" || mode == "clamp" || mode == "wrap" ||
                mode == "reflect101",
            "correlation boundary mode");
    boundary = mode == "zero"    ? Boundary::Zero
               : mode == "clamp" ? Boundary::Clamp
               : mode == "wrap"  ? Boundary::Wrap
                                 : Boundary::Reflect;
    const auto norm = text(p, "normalization", "none");
    require(norm == "none" || norm == "sum" || norm == "l2",
            "correlation normalization");
    normalization = norm == "none" ? 0 : norm == "sum" ? 1 : 2;
    scratch_bytes =
        sizeof(CorrelationScratch) + kh * kw * sizeof(numeric_ops::BinaryParts);
    add_output(this, tensor_schema(type, source, true, true));
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    std::vector<ReadRegion> result;
    for (const auto y :
         mapped_support(region.dimensions()[0], height, kh / 2, boundary)) {
      for (const auto x :
           mapped_support(region.dimensions()[1], width, kw / 2, boundary)) {
        result.push_back({0, 0, Region({y, x, region.dimensions()[2]})});
      }
    }
    result.push_back({1, 0, Region::whole({kh, kw})});
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* scratch =
        new (invocation.scratch[0].allocation) CorrelationScratch(work);
    struct Destroy {
      CorrelationScratch* p;
      ~Destroy() { p->~CorrelationScratch(); }
    } destroy{scratch};
    auto& m = scratch->math;
    auto* dot = &scratch->dot;
    auto* norm = &scratch->norm;
    auto& bins = scratch->bins;
    auto* weights = reinterpret_cast<numeric_ops::BinaryParts*>(scratch + 1);
    const auto rational = [](const numeric_ops::RatioWorkspace& value) {
      Rational result;
      std::copy(value.numerator.words.begin(), value.numerator.words.end(),
                result.n.words.begin());
      result.d.words.fill(0);
      result.d.words[2148 / 64] = UINT64_C(1) << (2148 % 64);
      result.negative = value.negative;
      return result;
    };
    const auto one = numeric_ops::BinaryParts::decode(bits(1), false);
    for (std::uint64_t y = 0; y < kh; ++y) {
      for (std::uint64_t x = 0; x < kw; ++x) {
        work.poll(16);
        auto* w = new (weights + y * kw + x)
            numeric_ops::BinaryParts(numeric_ops::BinaryParts::decode(
                bits(reader.number(1, 0, {y, x})), false));
        if (normalization) {
          bins.add(*w, normalization == 2 ? *w : one);
        }
      }
    }
    if (normalization) {
      work.poll(6 * 68);
      bins.finish(norm);
    }
    const auto denominator =
        normalization == 2 ? rational(*norm) : m.exact.integer(1);
    const bool narrow = type == ElementType::Float32;
    samples(invocation.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      m.exact.reset();
      bins.reset();
      for (std::uint64_t y = 0; y < kh; ++y) {
        const auto sy = mapped(static_cast<std::int64_t>(at[0] + y) -
                                   static_cast<std::int64_t>(kh / 2),
                               height, boundary);
        for (std::uint64_t x = 0; x < kw; ++x) {
          work.poll(16);
          const auto& w = weights[y * kw + x];
          if (!w.significand) {
            continue;
          }
          const auto sx = mapped(static_cast<std::int64_t>(at[1] + x) -
                                     static_cast<std::int64_t>(kw / 2),
                                 width, boundary);
          if (sy < 0 || sx < 0) {
            continue;
          }
          const auto v = numeric_ops::BinaryParts::decode(
              bits(reader.number(0, 0,
                                 {static_cast<std::uint64_t>(sy),
                                  static_cast<std::uint64_t>(sx), at[2]})),
              false);
          bins.add(w, v);
        }
      }
      work.poll(6 * 68);
      bins.finish(dot);
      std::uint64_t result;
      if (normalization == 0) {
        dot->denominator.words.fill(0);
        dot->denominator.words[0] = 1;
        if (numeric_ops::RatioWorkspace::top(dot->numerator) < 0) {
          result = 0;
        } else {
          result = take(dot->round(narrow, work, -2148));
        }
      } else if (normalization == 1) {
        dot->denominator = norm->numerator;
        dot->negative = dot->negative != norm->negative;
        result = take(dot->round(narrow, work, 0));
      } else {
        result = m.tangent(rational(*dot), denominator, narrow);
      }
      store(invocation.outputs[0], at, result);
    });
  }
};
std::shared_ptr<Program> make_noise(
    NoiseKind kind, const std::vector<OperationMetadata>& inputs,
    const Parameters& p, bool whole = false) {
  auto state = std::make_shared<Noise>(kind, p);
  state->whole_inputs = whole;
  if (kind == NoiseKind::Uniform || kind == NoiseKind::Gaussian) {
    Canvas canvas(p);
    state->height = canvas.height;
    state->width = canvas.width;
    if (kind == NoiseKind::Uniform) {
      state->channels = integer(p, "channels");
    } else {
      const auto dimensions = tensor(inputs[0]).sample_shape();
      require(dimensions.size() == 1, "Gaussian mean requires a vector");
      shape(inputs[0], dimensions);
      shape(inputs[1], dimensions);
      state->channels = dimensions[0];
    }
    require(state->channels && state->channels <= 65536,
            "noise channel capacity");
    add_output(state.get(),
               tensor_schema(state->type,
                             {state->height, state->width, state->channels},
                             true, true));
  } else {
    const auto& field = tensor(inputs[0]);
    auto dimensions = field.sample_shape();
    state->dimensions = kind == NoiseKind::Perlin ? 3 : 2;
    require(dimensions.size() >= 2 && dimensions.back() == state->dimensions &&
                floating(field.descriptor.element_type),
            "gradient noise requires coordinate tuples of matching dimension");
    dimensions.pop_back();
    add_output(state.get(), tensor_schema(state->type, dimensions));
  }
  return state;
}
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_noise(OperationRegistry* registry) {
  using generation::append;
  using generation::canvas_parameters;
  using generation::check;
  using generation::Correlation;
  using generation::make_noise;
  using generation::NoiseKind;
  using generation::operation;
  using generation::rng_parameters;
  using generation::tensor_schema;
  try {
    const auto prototype = tensor_schema(ElementType::Float64, {1});
    for (auto kind : {NoiseKind::Uniform, NoiseKind::Gaussian,
                      NoiseKind::Perlin, NoiseKind::Gradient}) {
      auto parameters =
          kind == NoiseKind::Uniform || kind == NoiseKind::Gaussian
              ? canvas_parameters()
              : std::vector<OperationParameterSpec>{
                    {"dtype", OperationParameterType::String, false}};
      if (kind != NoiseKind::Perlin) {
        append(&parameters, rng_parameters());
      }
      if (kind == NoiseKind::Uniform) {
        parameters.push_back({"channels", OperationParameterType::Int64});
      }
      if (kind == NoiseKind::Gradient) {
        parameters.push_back(
            {"period_x", OperationParameterType::Int64, false});
        parameters.push_back(
            {"period_y", OperationParameterType::Int64, false});
      }
      const unsigned count = kind == NoiseKind::Uniform  ? 1
                             : kind == NoiseKind::Perlin ? 2
                                                         : 3;
      for (unsigned profile = 0; profile < count; ++profile) {
        const char* suffix[] = {"strict", "accelerated_apple_silicon",
                                "accelerated_x86_64"};
        const auto key =
            kind == NoiseKind::Perlin
                ? std::string("noise.perlin2002_3d_v1_strict_cpu_") +
                      (profile ? "tiled" : "whole")
                : std::string("noise.") +
                      (kind == NoiseKind::Uniform    ? "uniform_philox"
                       : kind == NoiseKind::Gaussian ? "gaussian_box_muller"
                                                     : "gradient2d_philox") +
                      "_v1_" + suffix[profile];
        check(registry->register_operation(operation(
            key,
            kind == NoiseKind::Uniform    ? 0
            : kind == NoiseKind::Gaussian ? 2
                                          : 1,
            parameters, {{"values", prototype}},
            [kind, profile](const auto& inputs, const auto& p) {
              if (kind != NoiseKind::Perlin) {
                check(numeric_ops::sequence_profile_available(
                    static_cast<numeric_ops::SequenceProfile>(profile)));
              }
              return make_noise(kind, inputs, p,
                                kind == NoiseKind::Perlin && profile == 0);
            })));
      }
    }
    auto parameters = std::vector<OperationParameterSpec>{
        {"dtype", OperationParameterType::String, false},
        {"boundary", OperationParameterType::String},
        {"normalization", OperationParameterType::String, false},
        {"max_kernel_extent", OperationParameterType::Int64, false}};
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned profile = 0; profile < 3; ++profile) {
      check(registry->register_operation(
          operation(std::string("noise.correlate_kernel_v1_") + suffix[profile],
                    2, parameters, {{"values", prototype}},
                    [profile](const auto& inputs, const auto& p) {
                      check(numeric_ops::sequence_profile_available(
                          static_cast<numeric_ops::SequenceProfile>(profile)));
                      return std::make_shared<Correlation>(inputs, p);
                    })));
    }
    auto white_parameters = canvas_parameters();
    append(&white_parameters, rng_parameters());
    white_parameters.push_back({"channels", OperationParameterType::Int64});
    for (unsigned profile = 0; profile < 3; ++profile) {
      check(registry->register_operation(operation(
          std::string("noise.grain_white_v1_") + suffix[profile], 0,
          white_parameters, {{"values", prototype}},
          [profile](const auto&, const auto& p) {
            check(numeric_ops::sequence_profile_available(
                static_cast<numeric_ops::SequenceProfile>(profile)));
            auto state =
                std::make_shared<generation::Noise>(NoiseKind::Gaussian, p);
            state->grain_white = true;
            generation::Canvas canvas(p);
            state->height = canvas.height;
            state->width = canvas.width;
            state->channels = generation::integer(p, "channels");
            generation::require(state->channels == 1 || state->channels == 3,
                                "grain white requires one or three channels");
            generation::add_output(
                state.get(),
                tensor_schema(state->type,
                              {state->height, state->width, state->channels},
                              true, true));
            return state;
          })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
