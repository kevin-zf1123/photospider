#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/fractal_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class FractalKind { Fbm, Turbulence, Ridged, Footprint };
struct Fractal final : Program {
  FractalKind kind;
  ElementType type;
  bool perlin, normalize, accelerated;
  unsigned octaves, dimensions;
  double frequency, lacunarity, gain;
  std::uint64_t seed, stream, frame;
  std::array<std::int64_t, 2> period;
  Fractal(FractalKind selected, const std::vector<OperationMetadata>& in,
          const Parameters& p, bool fast)
      : kind(selected),
        type(dtype(p)),
        perlin(text(p, "basis", "gradient2d_philox") == "perlin2002_3d"),
        normalize(text(p, "normalization", "amplitude_sum") == "amplitude_sum"),
        accelerated(fast),
        octaves(static_cast<unsigned>(integer(p, "octaves", 4))),
        dimensions(perlin ? 3 : 2),
        frequency(real(p, "frequency", 1)),
        lacunarity(real(p, "lacunarity", 2)),
        gain(real(p, "gain", .5)),
        seed(static_cast<std::uint64_t>(integer(p, "seed"))),
        stream(static_cast<std::uint64_t>(integer(p, "stream"))),
        frame(static_cast<std::uint64_t>(integer(p, "frame"))),
        period{integer(p, "period_x"), integer(p, "period_y")} {
    const auto basis = text(p, "basis", "gradient2d_philox");
    require(basis == "gradient2d_philox" || basis == "perlin2002_3d",
            "unknown fractal basis");
    const auto normalization = text(p, "normalization", "amplitude_sum");
    require(normalization == "none" || normalization == "amplitude_sum",
            "unknown fractal normalization");
    if (!perlin) {
      require(p.count("seed") && p.count("stream"),
              "gradient2d fractal requires explicit seed and stream");
    } else {
      require(!p.count("seed") && !p.count("stream") && !p.count("frame") &&
                  !p.count("period_x") && !p.count("period_y"),
              "Perlin2002 has a fixed permutation and period");
    }
    const auto& coordinates = tensor(in[0], true);
    auto shape = coordinates.sample_shape();
    require(floating(coordinates.descriptor.element_type) &&
                shape.size() >= 2 && shape.back() == dimensions,
            "fractal coordinates require floating S+[basis dimension]");
    shape.pop_back();
    if (kind == FractalKind::Footprint) {
      const auto& footprint = tensor(in[1], true);
      require(floating(footprint.descriptor.element_type) &&
                  footprint.sample_shape() == shape,
              "fractal footprint requires floating sample shape S");
    }
    add_output(this, tensor_schema(type, shape));
    scratch_bytes = sizeof(Exact);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    auto coordinates = region.dimensions();
    coordinates.push_back({0, dimensions});
    std::vector<ReadRegion> reads{{0, 0, Region(std::move(coordinates))}};
    if (kind == FractalKind::Footprint)
      reads.push_back({1, 0, region});
    return reads;
  }
  std::optional<std::uint64_t> fast_sample(
      const Reader& reader, const ps_kernel_mutable_view_v8& output,
      const Coordinate& at, const Work& work, bool narrow) const {
    std::array<Fast, 3> query;
    for (unsigned axis = 0; axis < dimensions; ++axis) {
      auto coordinate = at;
      coordinate[output.layout->rank] = axis;
      query[axis] = Fast::point(reader.number(0, 0, coordinate));
    }
    const auto footprint = Fast::point(
        kind == FractalKind::Footprint ? reader.number(1, 0, at) : 0);
    const auto one = Fast::point(1), two = Fast::point(2);
    auto scale = Fast::point(frequency), amplitude = one;
    auto sum = Fast::point(0), denominator = Fast::point(0);
    const auto count = gain == 0 ? 1U : octaves;
    for (unsigned octave = 0; octave < count; ++octave) {
      work.poll(24);
      denominator = denominator + amplitude;
      auto weight = amplitude;
      bool zero = false;
      if (kind == FractalKind::Footprint) {
        auto attenuation = two - two * footprint * scale;
        if (!attenuation.finite())
          return {};
        zero = attenuation.high <= 0;
        attenuation = {std::max(0.0, std::min(1.0, attenuation.low)),
                       std::max(0.0, std::min(1.0, attenuation.high))};
        weight = amplitude * attenuation;
      }
      if (!zero) {
        std::array<Fast, 3> q;
        for (unsigned axis = 0; axis < dimensions; ++axis)
          q[axis] = query[axis] * scale;
        auto basis = fast_basis(q, perlin, seed, stream, frame, period, work);
        if (!basis)
          return {};
        auto term = *basis;
        if (kind == FractalKind::Turbulence)
          term = fast_abs(term);
        if (kind == FractalKind::Ridged) {
          const auto ridge = one - fast_abs(term) * Fast::point(.5);
          term = ridge * ridge;
        }
        sum = sum + weight * term;
      }
      if (octave + 1 < count) {
        amplitude = amplitude * Fast::point(gain);
        scale = scale * Fast::point(lacunarity);
      }
    }
    return fast_result(normalize ? sum / denominator : sum, narrow,
                       accelerated);
  }
  // This selects an arithmetic tier for the consumed sample, not a validator.
  // Power-of-two octave scales preserve dyadic widths. At most 32 octaves,
  // degree 32 (the squared 3D basis), 248 amplitude bits, normalization and
  // final subnormal rounding fit 4096 bits for 56-bit fractions and nonnegative
  // frequency exponents; 8192 bits covers 116-bit fractions and exponent -8.
  unsigned exact_words(const Reader& reader, unsigned rank,
                       const Coordinate& at) const {
    const auto power = [](double x, int low, int high) {
      int exponent = 0;
      return std::frexp(x, &exponent) == .5 && exponent - 1 >= low &&
             exponent - 1 <= high;
    };
    if (octaves > 32 || !power(frequency, -8, 8) || !power(lacunarity, 1, 8) ||
        (gain != 0 && !power(gain, -8, 0)))
      return 384;
    unsigned fractional = 0;
    const auto inspect = [&](double value) {
      if (std::abs(value) > 0x1p64 || !std::isfinite(value))
        return false;
      auto parts = numeric_ops::BinaryParts::decode(bits(value), false);
      if (parts.significand) {
        const int exponent =
            parts.exponent + __builtin_ctzll(parts.significand);
        fractional =
            std::max(fractional, static_cast<unsigned>(std::max(0, -exponent)));
      }
      return true;
    };
    for (unsigned axis = 0; axis < dimensions; ++axis) {
      auto q = at;
      q[rank] = axis;
      if (!inspect(reader.number(0, 0, q)))
        return 384;
    }
    if (kind == FractalKind::Footprint && !inspect(reader.number(1, 0, at)))
      return 384;
    if (fractional <= 56 && frequency >= 1)
      return 64;
    return fractional <= 116 ? 128 : 384;
  }
  template <unsigned Words>
  std::uint64_t exact_sample(const Reader& reader,
                             const ps_kernel_mutable_view_v8& output,
                             const Coordinate& at, const Work& work,
                             void* memory, bool narrow) const {
    using E = model_ops::RationalMathStorage<Words>;
    using R = typename E::Rational;
    auto* storage = new (memory) E(numeric_ops::SequenceProfile::Strict);
    struct Destroy {
      E* p;
      ~Destroy() { p->~E(); }
    } destroy{storage};
    auto& e = *storage;
    e.bind(work);
    const auto read = [&](unsigned port, const Coordinate& q) {
      return e.binary(
          reader.bits(port, 0, q),
          reader.kernel.groups[reader.kernel.input_base[port]].type ==
              ElementType::Float32);
    };
    std::array<R, 3> query;
    for (unsigned axis = 0; axis < dimensions; ++axis) {
      auto q = at;
      q[output.layout->rank] = axis;
      query[axis] = read(0, q);
    }
    const auto footprint =
        kind == FractalKind::Footprint ? read(1, at) : e.integer(0);
    const auto one = e.integer(1);
    const auto two = e.integer(2);
    const auto lac = e.binary(bits(lacunarity));
    const auto gain_value = e.binary(bits(gain));
    auto scale = e.binary(bits(frequency));
    auto amplitude = one, sum = e.integer(0), denominator = e.integer(0);
    for (unsigned octave = 0; octave < octaves; ++octave) {
      work.poll();
      if (E::zero(amplitude))
        break;
      denominator = e.add(denominator, amplitude);
      auto weight = amplitude;
      if (kind == FractalKind::Footprint) {
        auto attenuation =
            e.subtract(two, e.multiply(two, e.multiply(footprint, scale)));
        if (e.order(attenuation, e.integer(0)) <= 0)
          weight = e.integer(0);
        else if (e.order(attenuation, one) < 0)
          weight = e.multiply(weight, attenuation);
      }
      if (!E::zero(weight)) {
        std::array<R, 3> q;
        for (unsigned axis = 0; axis < dimensions; ++axis)
          q[axis] = e.multiply(query[axis], scale);
        auto term = fractal_basis(e, q, perlin, seed, stream, frame, period);
        if (kind == FractalKind::Turbulence)
          term = e.absolute(term);
        if (kind == FractalKind::Ridged) {
          const auto ridge = e.subtract(one, e.divide(e.absolute(term), two));
          term = e.multiply(ridge, ridge);
        }
        sum = e.add(sum, e.multiply(weight, term));
      }
      check(e.status());
      if (octave + 1 < octaves) {
        amplitude = e.multiply(amplitude, gain_value);
        if (!E::zero(amplitude))
          scale = e.multiply(scale, lac);
      }
    }
    if (normalize)
      sum = e.divide(sum, denominator);
    if (E::zero(sum))
      sum.negative = false;
    const auto result = e.rounded(sum, narrow);
    check(e.status());
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    const auto& output = invocation.outputs[0];
    const bool narrow = type == ElementType::Float32;
    samples(output, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      if (narrow || accelerated) {
        if (const auto value = fast_sample(reader, output, at, work, narrow)) {
          store(output, at, *value);
          return;
        }
      }
      const auto words = exact_words(reader, output.layout->rank, at);
      auto* memory = invocation.scratch[0].allocation;
      const auto result =
          words == 64
              ? exact_sample<64>(reader, output, at, work, memory, narrow)
          : words == 128
              ? exact_sample<128>(reader, output, at, work, memory, narrow)
              : exact_sample<384>(reader, output, at, work, memory, narrow);
      store(output, at, result);
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_fractals(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    const std::vector<OperationParameterSpec> parameters{
        {"dtype", OperationParameterType::String, false},
        {"basis", OperationParameterType::String, false},
        {"normalization", OperationParameterType::String, false},
        {"octaves", OperationParameterType::Int64, false},
        {"frequency", OperationParameterType::Float64, false},
        {"lacunarity", OperationParameterType::Float64, false},
        {"gain", OperationParameterType::Float64, false},
        {"seed", OperationParameterType::Int64, false},
        {"stream", OperationParameterType::Int64, false},
        {"frame", OperationParameterType::Int64, false},
        {"period_x", OperationParameterType::Int64, false},
        {"period_y", OperationParameterType::Int64, false}};
    const char* names[] = {"fbm_sum", "turbulence_sum", "ridged_sum",
                           "footprint_fbm"};
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned kind = 0; kind < 4; ++kind) {
      for (unsigned profile = 0; profile < 3; ++profile) {
        check(registry->register_operation(operation(
            std::string("noise.") + names[kind] + "_v1_" + suffix[profile],
            kind == 3 ? 2 : 1, parameters,
            {{"values", tensor_schema(ElementType::Float64, {1})}},
            [kind, profile](const auto& inputs, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<Fractal>(static_cast<FractalKind>(kind),
                                               inputs, p, profile != 0);
            })));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
