#include <cmath>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/advanced.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct GaborKernel final : Program {
  double frequency, theta, phase, sigma, aspect;
  std::uint64_t ry, rx;
  bool remove, normalize;
  bool cache_taps = false;
  GaborKernel(Profile profile, const Parameters& p)
      : frequency(real(p, "frequency")),
        theta(real(p, "theta")),
        phase(real(p, "phase")),
        sigma(real(p, "sigma")),
        aspect(real(p, "aspect")),
        ry(integer(p, "radius_y")),
        rx(integer(p, "radius_x")),
        remove(text(p, "dc") == "remove"),
        normalize(text(p, "normalization") == "l1_envelope") {
    check(numeric_ops::sequence_profile_available(profile));
    const auto h = radius_extent(integer(p, "radius_y"));
    const auto w = radius_extent(integer(p, "radius_x"));
    parameter(text(p, "dc") == "keep" || remove, "Gabor dc policy");
    parameter(text(p, "normalization") == "none" || normalize,
              "Gabor normalization");
    size_product({h, w});
    add_output(this, tensor_schema(ElementType::Float64, {h, w}));
    add_output(this, tensor_schema(ElementType::Float64, {h, w}));
    collective = true;
    cache_taps = (remove || normalize) && h * w <= 256;
    scratch_bytes = sizeof(generation::Math) +
                    (22 + (cache_taps ? 4 * h * w : 0)) *
                        sizeof(generation::Directed::Number) +
                    64;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    return {};
  }
  void envelope_exp(generation::Math& m, generation::Interval out,
                    generation::Interval argument) const {
    auto& d = m.d;
    generation::Directed::Frame frame(d);
    auto limit = d.interval();
    d.integer(limit, -1024);
    if (d.compare(argument.low, limit.low) > 0) {
      m.functions.exponential(out, argument);
      return;
    }
    // The exponential is an internal exact factor, so its underflow cannot
    // terminate the enclosing DC-removal/normalization expression. e>2 gives
    // an arbitrarily refinable [0,2^-cutoff] bound for the far negative tail.
    const auto cutoff = static_cast<int>(d.precision + 1152);
    d.integer(limit, -cutoff);
    if (d.compare(argument.high, limit.low) <= 0) {
      d.dyadic(out, 1, -cutoff);
      d.integer(out.low, 0);
      return;
    }
    auto reduced = d.interval();
    d.scale(reduced, argument, -4);
    m.functions.exponential(out, reduced);
    for (unsigned i = 0; i < 4 && d.active(); ++i)
      d.multiply(out, out, out);
  }
  void tap(generation::Math& m, std::int64_t y, std::int64_t x,
           generation::Interval envelope, generation::Interval real_part,
           generation::Interval imag_part,
           generation::Directed::Number* constants, unsigned& precision,
           bool unphased = false, int* real_sign = nullptr,
           int* imag_sign = nullptr) const {
    auto& d = m.d;
    generation::Directed::Frame frame(d);
    auto t = d.interval(), st = d.interval(), ct = d.interval();
    auto xx = d.interval(), yy = d.interval(), a = d.interval(),
         b = d.interval();
    if (precision == d.precision) {
      d.copy(st, {constants[0], constants[1]});
      d.copy(ct, {constants[2], constants[3]});
    } else {
      m.enclose(t, m.value(theta));
      m.sincos(st, ct, t);
      d.copy({constants[0], constants[1]}, st);
      d.copy({constants[2], constants[3]}, ct);
      m.enclose(t, m.sq(m.value(aspect)));
      d.copy({constants[4], constants[5]}, t);
      m.enclose(t, m.scale(m.sq(m.value(sigma)), -2));
      d.copy({constants[6], constants[7]}, t);
      d.constant(t, true);
      m.enclose(a, m.scale(m.value(frequency), 2));
      d.multiply(t, t, a);
      d.copy({constants[8], constants[9]}, t);
      m.enclose(t, m.value(phase));
      d.copy({constants[10], constants[11]}, t);
      m.sincos(a, b, t);
      d.copy({constants[12], constants[13]}, a);
      d.copy({constants[14], constants[15]}, b);
      if (d.active())
        precision = d.precision;
    }
    m.enclose(a, m.exact.integer(x));
    m.enclose(b, m.exact.integer(y));
    d.multiply(xx, a, ct);
    d.multiply(t, b, st);
    d.add(xx, xx, t);
    d.multiply(yy, b, ct);
    d.multiply(t, a, st);
    d.subtract(yy, yy, t);
    d.multiply(a, xx, xx);
    d.copy(t, {constants[4], constants[5]});
    d.multiply(b, yy, yy);
    d.multiply(b, b, t);
    d.add(a, a, b);
    d.copy(t, {constants[6], constants[7]});
    d.divide(a, a, t);
    envelope_exp(m, envelope, a);
    bool exact_phase = false;
    std::int64_t parity = 0;
    if ((unphased || phase == 0) && (frequency == 0 || theta == 0)) {
      const auto turns =
          m.mul(m.value(frequency), m.exact.integer(theta == 0 ? x : 0));
      const auto twice = m.scale(turns, 2);
      const auto integer =
          static_cast<std::int64_t>(number(m.rounded(twice, false)));
      if (m.exact.order(twice, m.exact.integer(integer)) == 0) {
        exact_phase = true;
        parity = integer & 1;
      }
    }
    if (exact_phase) {
      m.enclose(a, m.exact.integer(parity ? -1 : 1));
      m.enclose(b, m.exact.integer(0));
    } else {
      d.copy(t, {constants[8], constants[9]});
      d.multiply(t, t, xx);
      if (!unphased) {
        d.copy(a, {constants[10], constants[11]});
        d.add(t, t, a);
      }
      m.sincos(b, a, t);
    }
    if (real_sign || imag_sign) {
      auto zero = d.interval();
      d.integer(zero, 0);
      const auto sign = [&](generation::Interval v) {
        return d.compare(v.high, zero.low) < 0   ? -1
               : d.compare(v.low, zero.high) > 0 ? 1
                                                 : 0;
      };
      if (real_sign)
        *real_sign = sign(a);
      if (imag_sign)
        *imag_sign = sign(b);
    }
    d.multiply(real_part, envelope, a);
    d.multiply(imag_part, envelope, b);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    generation::MathArena arena(inv.scratch[0].allocation, work);
    auto& m = *arena;
    m.exact.bind(fuel);
    m.d.consume = core_internal::WorkConsumer(fuel);
    auto* cache =
        reinterpret_cast<generation::Directed::Number*>(arena.value + 1);
    for (std::uint64_t i = 0;
         i < 22 + (cache_taps ? 4 * (2 * ry + 1) * (2 * rx + 1) : 0); ++i) {
      fuel.poll(generation::Directed::kWords);
      new (cache + i) generation::Directed::Number{};
    }
    bool cached = false;
    unsigned constant_precision = 0;
    for (unsigned output_index = 0; output_index < inv.output_count;
         ++output_index) {
      const bool imaginary = kernel.writes[output_index].first == 1;
      const auto& output = inv.outputs[output_index];
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        fuel.poll();
        if ((remove && ry == 0 && rx == 0) ||
            (imaginary && frequency == 0 && phase == 0)) {
          store(output, at, 0);
          return;
        }
        std::optional<std::uint64_t> signed_tiny;
        const auto result = m.certify(false, [&](generation::Interval out) {
          auto& d = m.d;
          auto e = d.interval(), a = d.interval(), b = d.interval();
          int real_sign = 0, imag_sign = 0;
          const bool defer_tap = cache_taps && !cached && d.precision == 128;
          if (defer_tap) {
            d.integer(a, 0);
            d.integer(b, 0);
          } else if (cache_taps && cached && d.precision == 128) {
            auto* raw = cache + 22 + 4 * (at[0] * (2 * rx + 1) + at[1]);
            d.copy(a, {raw[0], raw[1]});
            d.copy(b, {raw[2], raw[3]});
          } else {
            tap(m, static_cast<std::int64_t>(at[0]) - ry,
                static_cast<std::int64_t>(at[1]) - rx, e, a, b, cache + 6,
                constant_precision, false, &real_sign, &imag_sign);
          }
          d.copy(out, imaginary ? b : a);
          const auto certify_tiny = [&]() {
            const auto sign = imaginary ? imag_sign : real_sign;
            if (remove || !sign || !d.active())
              return;
            const auto low = d.rounded(out.low, false),
                       high = d.rounded(out.high, false);
            if (d.active() &&
                ((low | high) & UINT64_C(0x7fffffffffffffff)) == 0) {
              // E and its normalization mass are strictly positive. A certified
              // carrier sign resolves an underflow interval touching exact
              // zero.
              signed_tiny = sign < 0 ? UINT64_C(0x8000000000000000) : 0;
              d.integer(out, 0);
            }
          };
          if (!remove && !normalize) {
            certify_tiny();
            return;
          }
          auto sum = d.interval(), other_sum = d.interval(),
               mass = d.interval(), constant = d.interval();
          if (d.precision == 128 && cached) {
            d.copy(sum, imaginary ? generation::Interval{cache[2], cache[3]}
                                  : generation::Interval{cache[0], cache[1]});
            d.copy(mass, {cache[4], cache[5]});
          } else {
            m.enclose(sum, m.exact.integer(0));
            m.enclose(other_sum, m.exact.integer(0));
            m.enclose(mass, m.exact.integer(0));
            for (std::uint64_t y = 0; y <= 2 * ry; ++y)
              for (std::uint64_t x = 0; x <= 2 * rx; ++x) {
                fuel.poll();
                const auto index = y * (2 * rx + 1) + x;
                const auto mirror = (2 * ry + 1) * (2 * rx + 1) - 1 - index;
                if (cache_taps && index > mirror)
                  continue;
                tap(m, static_cast<std::int64_t>(y) - ry,
                    static_cast<std::int64_t>(x) - rx, e, a, b, cache + 6,
                    constant_precision, cache_taps);
                if (cache_taps) {
                  generation::Directed::Frame frame(d);
                  auto ac = d.interval(), bs = d.interval(), as = d.interval(),
                       bc = d.interval();
                  auto reverse_real = d.interval(), reverse_imag = d.interval();
                  d.multiply(ac, a, {cache[20], cache[21]});
                  d.multiply(bs, b, {cache[18], cache[19]});
                  d.multiply(as, a, {cache[18], cache[19]});
                  d.multiply(bc, b, {cache[20], cache[21]});
                  d.subtract(a, ac, bs);
                  d.add(b, as, bc);
                  d.add(reverse_real, ac, bs);
                  d.subtract(reverse_imag, as, bc);
                  if (d.precision == 128) {
                    auto* raw = cache + 22 + 4 * index;
                    d.copy({raw[0], raw[1]}, a);
                    d.copy({raw[2], raw[3]}, b);
                    if (index != mirror) {
                      raw = cache + 22 + 4 * mirror;
                      d.copy({raw[0], raw[1]}, reverse_real);
                      d.copy({raw[2], raw[3]}, reverse_imag);
                    }
                  }
                  if (index != mirror) {
                    d.add(sum, sum, reverse_real);
                    d.add(other_sum, other_sum, reverse_imag);
                    d.add(mass, mass, e);
                  }
                }
                d.add(sum, sum, a);
                d.add(other_sum, other_sum, b);
                d.add(mass, mass, e);
              }
            if (remove) {
              m.enclose(constant, m.exact.integer((2 * ry + 1) * (2 * rx + 1)));
              d.divide(sum, sum, constant);
              d.divide(other_sum, other_sum, constant);
            }
            if (normalize) {
              m.enclose(constant, m.exact.integer(1));
              d.divide(mass, constant, mass);
            }
            if (d.precision == 128 && d.active()) {
              d.copy({cache[0], cache[1]}, sum);
              d.copy({cache[2], cache[3]}, other_sum);
              d.copy({cache[4], cache[5]}, mass);
              cached = d.active();
            }
            if (imaginary)
              d.copy(sum, other_sum);
          }
          if (defer_tap) {
            auto* raw = cache + 22 + 4 * (at[0] * (2 * rx + 1) + at[1]);
            d.copy(out, imaginary ? generation::Interval{raw[2], raw[3]}
                                  : generation::Interval{raw[0], raw[1]});
          }
          if (remove && !(imaginary && phase == 0))
            d.subtract(out, out, sum);
          if (normalize)
            d.multiply(out, out, mass);
          certify_tiny();
        });
        store(output, at, signed_tiny ? signed_tiny.value() : result);
      });
    }
    fuel.flush();
  }
};
struct GaborResponse final : Program {
  std::shared_ptr<Filter> real_filter, imag_filter;
  GaborResponse(Profile profile, const std::vector<OperationMetadata>& in,
                const Parameters& p) {
    const auto &a = tensor(in[1]), &b = tensor(in[2]);
    require(a.sample_shape() == b.sample_shape() &&
                a.descriptor.element_type == ElementType::Float64 &&
                b.descriptor.element_type == ElementType::Float64,
            "Gabor kernels require matching Float64 shapes");
    auto args = p;
    args["boundary"] = std::string("reflect_half");
    args["cval"] = 0.0;
    args["shape"] = std::string("same");
    args["normalization"] = std::string("none");
    args["bias"] = 0.0;
    const std::vector<OperationMetadata> r{in[0], in[1]}, i{in[0], in[2]};
    real_filter = std::make_shared<Filter>(Kind::Correlate, profile, r, args);
    imag_filter = std::make_shared<Filter>(Kind::Correlate, profile, i, args);
    real_filter->inputs = r;
    imag_filter->inputs = i;
    if (p.count("expected_shape")) {
      std::string shape;
      for (auto n : tensor(in[0], true).sample_shape()) {
        if (!shape.empty())
          shape += ',';
        shape += std::to_string(n);
      }
      require(text(p, "expected_shape") == shape,
              "Gabor directory/source shape mismatch");
    }
    if (p.count("expected_dtype"))
      require(text(p, "expected_dtype") ==
                  (tensor(in[0], true).descriptor.element_type ==
                           ElementType::Float32
                       ? "float32"
                       : "float64"),
              "Gabor directory/source dtype mismatch");
    add_output(this, *real_filter->outputs[0].result_schema);
    add_output(this, *imag_filter->outputs[0].result_schema);
  }
  std::vector<ReadRegion> support(unsigned port,
                                  const Region& r) const override {
    auto result = (port ? imag_filter : real_filter)->support(0, r);
    for (auto& read : result)
      if (read.port == 1)
        read.port = port + 1;
    return result;
  }
  std::uint64_t task_scratch(unsigned port, const Region& r) const override {
    return (port ? imag_filter : real_filter)->task_scratch(0, r);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const unsigned port = kernel.writes[0].first;
    auto mapped = kernel;
    mapped.input_base = {kernel.input_base[0], kernel.input_base[port + 1]};
    mapped.writes = {{0, 0}};
    const Work work{inv, work_limit};
    (port ? imag_filter : real_filter)->run_with_work(mapped, inv, work);
  }
};
}  // namespace
OperationDefinition make_gabor_kernel(const std::string& key, Profile profile) {
  return generation::operation(
      key, 0,
      {{"frequency", Type::Float64},
       {"theta", Type::Float64},
       {"phase", Type::Float64},
       {"sigma", Type::Float64},
       {"aspect", Type::Float64},
       {"radius_y", Type::Int64},
       {"radius_x", Type::Int64},
       {"dc", Type::String},
       {"normalization", Type::String}},
      {{"kernel_real", tensor_schema(ElementType::Float64, {1, 1})},
       {"kernel_imag", tensor_schema(ElementType::Float64, {1, 1})}},
      [profile](const auto&, const auto& p) {
        return std::make_shared<GaborKernel>(profile, p);
      });
}
OperationDefinition make_gabor_response(const std::string& key,
                                        Profile profile) {
  return generation::operation(
      key, 3,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"anchor_y", Type::Int64},
       {"anchor_x", Type::Int64},
       {"expected_shape", Type::String, false},
       {"expected_dtype", Type::String, false}},
      {{"response_real", tensor_schema(ElementType::Float64, {1, 1})},
       {"response_imag", tensor_schema(ElementType::Float64, {1, 1})}},
      [profile](const auto& in, const auto& p) {
        return std::make_shared<GaborResponse>(profile, in, p);
      });
}
}  // namespace ps::plugin_internal::filter
