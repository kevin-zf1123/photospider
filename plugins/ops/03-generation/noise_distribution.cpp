#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
using SmallDirected = numeric_ops::DirectedIntervalStorage<8, true>;
struct DistributionScratch final {
  Math math;
  SmallDirected small{numeric_ops::SequenceProfile::Strict};
  std::array<double, 64> uniforms{}, logarithms{};
  explicit DistributionScratch(const Work& work, std::uint64_t refinements)
      : math(work, refinements) {
    small.consume = core_internal::WorkConsumer(work);
  }
};
struct FastPoisson final {
  using Value = Fast;
  Value value(double x) { return Fast::point(x); }
  void copy(Value& out, Value in) { out = in; }
  void add(Value& out, Value a, Value b) { out = a + b; }
  void sub(Value& out, Value a, Value b) { out = a - b; }
  void mul(Value& out, Value a, Value b) { out = a * b; }
  void div(Value& out, Value a, Value b) { out = a / b; }
  void divisor(Value& out, Value a, std::uint64_t n) { out = a / value(n); }
  void positive_tail(Value& out, Value in) { out = {0, in.high}; }
  bool below(Value a, Value b) {
    return a.finite() && b.finite() && a.high < b.low;
  }
  bool reached(Value a, Value b) {
    return a.finite() && b.finite() && a.low >= b.high;
  }
  bool active() const { return true; }
};
template <class D>
struct DirectedPoisson final {
  using Value = typename D::Interval;
  D& d;
  Value value(double x) {
    auto v = d.interval();
    d.raw(v, bits(x), false);
    return v;
  }
  void copy(Value out, Value in) { d.copy(out, in); }
  void add(Value out, Value a, Value b) { d.add(out, a, b); }
  void sub(Value out, Value a, Value b) { d.subtract(out, a, b); }
  void mul(Value out, Value a, Value b) { d.multiply(out, a, b); }
  void div(Value out, Value a, Value b) { d.divide(out, a, b); }
  void divisor(Value out, Value a, std::uint64_t n) {
    d.divide_small(out, a, n);
  }
  void positive_tail(Value out, Value in) {
    d.copy(out.high, in.high);
    d.integer(out.low, 0);
  }
  bool below(Value a, Value b) {
    const auto order = d.compare(a.high, b.low);
    return active() && order < 0;
  }
  bool reached(Value a, Value b) {
    const auto order = d.compare(a.low, b.high);
    return active() && order >= 0;
  }
  bool active() {
    if (d.fault == D::Fault::Failure)
      check(d.status());
    return d.active();
  }
};
// Normalize weights about floor(lambda), where w_mode=1. No exp(-lambda),
// factorial or saturated floating CDF is formed. Beyond each visited end,
// monotone consecutive ratios give a geometric upper bound on the entire tail.
template <class A>
std::optional<std::uint64_t> poisson_quantile(A& a, const Work& work,
                                              double lambda, double u,
                                              std::uint64_t limit,
                                              unsigned cutoff_bits) {
  const auto mode = static_cast<std::uint64_t>(std::floor(lambda));
  auto rate = a.value(lambda), one = a.value(1), epsilon = a.value(0);
  if constexpr (std::is_same_v<A, FastPoisson>)
    epsilon = a.value(std::ldexp(1.0, -static_cast<int>(cutoff_bits)));
  else
    a.d.dyadic(epsilon, 1, -static_cast<int>(cutoff_bits));
  auto term = a.value(1), total = a.value(1), tail = a.value(0);
  auto index = a.value(mode), denominator = a.value(0), temporary = a.value(0);
  auto left = a.value(0), right = a.value(0), previous = a.value(0);
  auto counter = a.value(0), source = a.value(u), target = a.value(0);
  auto cdf = a.value(0);
  std::uint64_t low = mode;
  while (low) {
    work.poll(16);
    // Integer loop values are exactly representable in the admitted rate range.
    if constexpr (std::is_same_v<A, FastPoisson>) {
      index = a.value(low);
      counter = a.value(low - 1);
    } else {
      a.d.integer(index, low);
      a.d.integer(counter, low - 1);
    }
    a.sub(denominator, rate, counter);
    a.mul(temporary, term, index);
    a.div(tail, temporary, denominator);
    if (!a.active())
      return {};
    if (a.below(tail, epsilon))
      break;
    a.div(term, temporary, rate);
    --low;
    a.add(total, total, term);
  }
  if (!low)
    a.copy(tail, cdf);
  a.positive_tail(left, tail);
  // Start the quantile walk at the mode. Reconstructing upward from the
  // tiny leftmost weight amplifies fixed-point absolute rounding errors.
  a.add(cdf, total, left);
  a.copy(term, one);
  std::uint64_t high = mode;
  for (;;) {
    work.poll(16);
    if constexpr (std::is_same_v<A, FastPoisson>)
      index = a.value(high + 1);
    else
      a.d.integer(index, high + 1);
    a.sub(denominator, index, rate);
    a.mul(temporary, term, rate);
    a.div(tail, temporary, denominator);
    if (!a.active())
      return {};
    if (a.below(tail, epsilon))
      break;
    ++high;
    a.divisor(term, temporary, high);
    a.add(total, total, term);
  }
  a.positive_tail(right, tail);
  a.add(total, total, left);
  a.add(total, total, right);
  a.mul(target, source, total);
  a.copy(term, one);
  const auto accepted = [&](std::uint64_t k) {
    if (k > limit)
      throw Status{ErrorCode::OperationFailed,
                   "Poisson count search not converged",
                   DomainErrorCode{UINT64_C(0x4e4f490002)}};
    return k;
  };
  if (!a.active())
    return {};
  if (a.reached(cdf, target)) {
    for (std::uint64_t k = mode;; --k) {
      work.poll(16);
      if (!k)
        return accepted(0);
      a.sub(previous, cdf, term);
      if (!a.active())
        return {};
      if (a.below(previous, target))
        return accepted(k);
      if (!a.reached(previous, target))
        return {};
      a.copy(cdf, previous);
      if constexpr (std::is_same_v<A, FastPoisson>)
        index = a.value(k);
      else
        a.d.integer(index, k);
      a.mul(temporary, term, index);
      a.div(term, temporary, rate);
      if (!a.active())
        return {};
    }
  }
  if (!a.below(cdf, target))
    return {};
  for (std::uint64_t k = mode;;) {
    work.poll(16);
    // CDF(k)<u proves that every count through k is insufficient.
    if (k >= limit)
      return accepted(limit + 1);
    ++k;
    a.mul(temporary, term, rate);
    a.divisor(term, temporary, k);
    a.add(cdf, cdf, term);
    if (!a.active())
      return {};
    if (a.reached(cdf, target))
      return accepted(k);
    if (!a.below(cdf, target))
      return {};
  }
}
struct Distribution final : Program {
  bool poisson, accelerated;
  ElementType type;
  std::int64_t origin_x, origin_y;
  std::uint64_t seed, stream, frame, looks, search_limit, refinements;
  Distribution(bool counts, bool fast, const std::vector<OperationMetadata>& in,
               const Parameters& p)
      : poisson(counts),
        accelerated(fast),
        type(counts ? ElementType::Int64 : dtype(p)),
        origin_x(integer(p, "origin_x")),
        origin_y(integer(p, "origin_y")),
        seed(integer(p, "seed")),
        stream(integer(p, "stream")),
        frame(integer(p, "frame")),
        looks(integer(p, "looks", 1)),
        search_limit(integer(p, "max_count_search", 2000000)),
        refinements(integer(p, "max_refinements", 4096)) {
    const auto& field = tensor(in[0]);
    const auto dimensions = field.sample_shape();
    require(dimensions.size() == 3 && floating(field.descriptor.element_type) &&
                dimensions[2] <= 65536,
            "distribution noise requires floating HWC input");
    if (p.count("height"))
      require(integer(p, "height") == static_cast<std::int64_t>(dimensions[0]),
              "distribution canvas height mismatch");
    if (p.count("width"))
      require(integer(p, "width") == static_cast<std::int64_t>(dimensions[1]),
              "distribution canvas width mismatch");
    add_output(this, tensor_schema(type, dimensions, true, true));
    scratch_bytes = sizeof(DistributionScratch);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    return {{0, 0, region}};
  }
  std::uint64_t quantile(DistributionScratch& scratch, const Work& work,
                         double lambda, double u) const {
    // exp(-lambda) >= 1-lambda > every open52 source when lambda <= 2^-54.
    if (lambda <= 0x1p-54)
      return 0;
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
    FastPoisson fast;
    if (const auto value =
            poisson_quantile(fast, work, lambda, u, search_limit, 80))
      return *value;
#endif
    for (unsigned precision = 128, attempt = 0;
         precision <= 4096 && attempt < refinements;
         precision *= 2, ++attempt) {
      std::optional<std::uint64_t> value;
      const unsigned cutoff = precision - 24;
      if (precision == 128) {
        scratch.small.reset();
        scratch.small.precision = precision;
        DirectedPoisson<SmallDirected> arithmetic{scratch.small};
        value =
            poisson_quantile(arithmetic, work, lambda, u, search_limit, cutoff);
      } else {
        auto& d = scratch.math.d;
        d.reset();
        d.precision = precision;
        DirectedPoisson<Directed> arithmetic{d};
        value =
            poisson_quantile(arithmetic, work, lambda, u, search_limit, cutoff);
      }
      if (value)
        return *value;
    }
    throw Status{ErrorCode::OperationFailed,
                 "Poisson quantile certification unresolved",
                 DomainErrorCode{UINT64_C(0x4e4f490002)}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    auto* scratch = new (invocation.scratch[0].allocation)
        DistributionScratch(work, refinements);
    struct Destroy {
      DistributionScratch* p;
      ~Destroy() { p->~DistributionScratch(); }
    } destroy{scratch};
    auto& m = scratch->math;
    const bool narrow = type == ElementType::Float32;
    samples(invocation.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      const double signal = reader.number(0, 0, at);
      if (!signal) {
        store(invocation.outputs[0], at, poisson ? 0 : convert(signal, narrow));
        return;
      }
      const auto x = origin_x + static_cast<std::int64_t>(at[1]);
      const auto y = origin_y + static_cast<std::int64_t>(at[0]);
      for (std::uint64_t j = 0; j < (poisson ? 1 : looks); ++j) {
        work.poll(10);
        const auto block =
            philox(x, y, at[2], poisson ? 8 : 9, seed, stream, frame, j);
        scratch->uniforms[j] =
            static_cast<double>(2 * (block[0] >> 12) + 1) * 0x1p-53;
      }
      if (poisson) {
        store(invocation.outputs[0], at,
              quantile(*scratch, work, signal, scratch->uniforms[0]));
        return;
      }
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
      if ((narrow || accelerated) &&
          numeric_ops::accelerated_math_available()) {
        work.poll(looks * 32);
        photospider_sleef_evaluate(1, scratch->uniforms.data(),
                                   scratch->uniforms.data(),
                                   scratch->logarithms.data(), looks);
        auto sum = Fast::point(0);
        for (std::uint64_t j = 0; j < looks; ++j)
          sum = sum -
                numeric_ops::accelerated_math_enclosure(scratch->logarithms[j]);
        const auto result =
            fast_result(Fast::point(signal) * sum / Fast::point(looks), narrow,
                        accelerated);
        if (result) {
          store(invocation.outputs[0], at, *result);
          return;
        }
      }
#endif
      m.exact.reset();
      const auto result = m.certify(narrow, [&](Interval out) {
        auto sum = m.d.interval(), logarithm = m.d.interval(),
             factor = m.d.interval();
        m.d.integer(sum, 0);
        for (std::uint64_t j = 0; j < looks; ++j) {
          work.poll();
          m.functions.functions.logarithm(
              logarithm, numeric_ops::BinaryParts::decode(
                             bits(scratch->uniforms[j]), false));
          m.d.subtract(sum, sum, logarithm);
        }
        m.d.divide_small(sum, sum, looks);
        m.d.raw(factor, bits(signal), false);
        m.d.multiply(out, factor, sum);
      });
      store(invocation.outputs[0], at, result);
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_distribution_noise(OperationRegistry* registry) {
  using generation::check;
  using generation::Distribution;
  using generation::operation;
  using generation::rng_parameters;
  using generation::tensor_schema;
  try {
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (bool poisson : {true, false}) {
      auto parameters = rng_parameters();
      for (const auto* name :
           {"origin_x", "origin_y", "height", "width", "max_refinements"})
        parameters.push_back({name, OperationParameterType::Int64, false});
      if (poisson) {
        parameters.push_back(
            {"max_count_search", OperationParameterType::Int64, false});
      } else {
        parameters.push_back({"looks", OperationParameterType::Int64});
        parameters.push_back({"dtype", OperationParameterType::String, false});
      }
      for (unsigned profile = 0; profile < (poisson ? 1U : 3U); ++profile)
        check(registry->register_operation(operation(
            std::string("noise.") +
                (poisson ? "poisson_icdf" : "speckle_integer_looks") + "_v1_" +
                suffix[profile],
            1, parameters,
            {{poisson ? "counts" : "values",
              tensor_schema(poisson ? ElementType::Int64 : ElementType::Float64,
                            {1, 1, 1})}},
            [poisson, profile](const auto& in, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<Distribution>(poisson, profile != 0, in,
                                                    p);
            })));
    }
    return Status::success();
  } catch (const Status& status) {
    return status;
  }
}
}  // namespace ps::plugin_internal
