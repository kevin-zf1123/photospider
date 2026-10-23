#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "03-generation/math.hpp"
#include "04-mask-morphology/gaussian_fixed.hpp"
#include "04-mask-morphology/spatial.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
using Math = generation::Math;
using Directed = generation::Directed;
using Interval = generation::Interval;
struct StoredInterval final {
  Directed::Number low, high;
  Interval view() { return {low, high}; }
};
enum class Boundary { Zero, Replicate, Reflect };
struct Gaussian final : Program {
  std::uint64_t height, width, ry, rx, coefficient_count;
  double sy, sx;
  Boundary boundary;
  ElementType type;
  Gaussian(const std::vector<OperationMetadata>& in, const Parameters& p)
      : sy(real(p, "sigma_y")), sx(real(p, "sigma_x")) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    type = s.descriptor.element_type;
    const auto y = integer(p, "radius_y"), x = integer(p, "radius_x");
    parameter(y >= 0 && x >= 0, "Gaussian radii determine static support");
    parameter((sy != 0 || y == 0) && (sx != 0 || x == 0),
              "zero Gaussian sigma requires zero radius");
    ry = y;
    rx = x;
    coefficient_count = bytes(WideUnsigned(ry) + rx + 2);
    const auto b = text(p, "boundary");
    parameter(b == "zero" || b == "replicate" || b == "reflect_half",
              "Gaussian boundary");
    boundary = b == "zero"        ? Boundary::Zero
               : b == "replicate" ? Boundary::Replicate
                                  : Boundary::Reflect;
    add_output(this, spatial_schema(type, {height, width}));
  }
  std::int64_t map(__int128 index, std::uint64_t extent) const {
    if (index >= 0 && index < extent) {
      return static_cast<std::int64_t>(index);
    }
    if (boundary == Boundary::Zero) {
      return -1;
    }
    if (boundary == Boundary::Replicate) {
      return static_cast<std::int64_t>(
          std::clamp(index, __int128(0), __int128(extent - 1)));
    }
    const auto period = __int128(2) * extent;
    auto value = index % period;
    if (value < 0) {
      value += period;
    }
    return static_cast<std::int64_t>(value < extent ? value
                                                    : period - 1 - value);
  }
  RegionDimension axis(RegionDimension q, std::uint64_t extent,
                       std::uint64_t radius) const {
    const __int128 first = __int128(q.offset) - radius;
    const __int128 last = __int128(q.offset) + q.extent - 1 + radius;
    if (boundary != Boundary::Reflect) {
      const auto lo = static_cast<std::uint64_t>(std::max(__int128(0), first));
      const auto hi =
          static_cast<std::uint64_t>(std::min(__int128(extent - 1), last));
      return {lo, hi - lo + 1};
    }
    if (last - first + 1 >= __int128(2) * extent) {
      return {0, extent};
    }
    auto lo = std::min(map(first, extent), map(last, extent));
    auto hi = std::max(map(first, extent), map(last, extent));
    auto block = first / extent;
    if (first % extent < 0) {
      --block;
    }
    for (auto next = (block + 1) * extent; next <= last; next += extent) {
      const auto at = map(next, extent);
      lo = std::min(lo, at);
      hi = std::max(hi, at);
    }
    return {static_cast<std::uint64_t>(lo),
            static_cast<std::uint64_t>(hi - lo + 1)};
  }
  Region source(const Region& q) const {
    const auto d = q.dimensions();
    return Region({axis(d[0], height, ry), axis(d[1], width, rx)});
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, source(q)}};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    if (!ry && !rx) {
      return 0;
    }
    const auto rows = source(q).dimensions()[0].extent;
    return bytes(sizeof(Math) +
                 WideUnsigned(coefficient_count) *
                     (sizeof(StoredInterval) + sizeof(GaussianBound)) +
                 WideUnsigned(rows) * q.dimensions()[1].extent *
                     sizeof(GaussianBound) +
                 128);
  }
  void weights(Math& m, double sigma, std::uint64_t radius,
               StoredInterval* stored, Interval denominator) const {
    auto& d = m.d;
    d.integer(denominator, 0);
    const auto s = m.value(sigma);
    const auto variance = m.scale(m.sq(s), 2);
    for (std::uint64_t j = 0; j <= radius; ++j) {
      m.work.poll();
      Directed::Frame frame(d);
      auto weight = d.interval(), argument = d.interval(), total = d.interval();
      if (!j) {
        d.integer(weight, 1);
      } else {
        auto exponent = m.div(m.sq(m.exact.integer(j)), variance);
        exponent.negative = true;
        m.enclose(argument, exponent);
        m.functions.exponential(weight, argument);
      }
      d.copy(stored[j].view(), weight);
      d.scale(total, weight, j ? 1 : 0);
      d.add(denominator, denominator, total);
    }
    check(m.exact.status());
    if (d.fault == Directed::Fault::Failure) {
      check(d.status());
    }
  }
  static GaussianBound fixed(Interval a) {
    const auto endpoint = [](const Directed::Number& n,
                             bool upper) -> WideUnsigned {
      if (n.negative) {
        return 0;
      }
      if (n.magnitude.words[2] > 1) {
        return kGaussianOne;
      }
      auto result = (WideUnsigned(n.magnitude.words[2]) << 120) |
                    (WideUnsigned(n.magnitude.words[1]) << 56) |
                    (n.magnitude.words[0] >> 8);
      if (upper && (n.magnitude.words[0] & 255)) {
        ++result;
      }
      return std::min(result, kGaussianOne);
    };
    return {endpoint(a.low, false), endpoint(a.high, true)};
  }
  void fixed_weights(Math& m, StoredInterval* raw, std::uint64_t radius,
                     Interval denominator, GaussianBound* result) const {
    auto& d = m.d;
    for (std::uint64_t j = 0; j <= radius; ++j) {
      m.work.poll();
      Directed::Frame frame(d);
      auto normalized = d.interval();
      d.divide(normalized, raw[j].view(), denominator);
      result[j] = fixed(normalized);
    }
  }
  double sample(const Reader& reader, __int128 y, __int128 x) const {
    const auto yy = map(y, height), xx = map(x, width);
    return yy < 0 || xx < 0 ? 0
                            : reader.value(0, 0,
                                           {static_cast<std::uint64_t>(yy),
                                            static_cast<std::uint64_t>(xx)});
  }
  std::optional<std::uint64_t> constant(const Reader& reader,
                                        const Coordinate& at,
                                        Visits& visits) const {
    const auto first =
        sample(reader, __int128(at[0]) - ry, __int128(at[1]) - rx);
    for (std::uint64_t y = 0; y <= 2 * ry; ++y) {
      for (std::uint64_t x = 0; x <= 2 * rx; ++x) {
        visits.step();
        if (sample(reader, __int128(at[0]) + y - ry,
                   __int128(at[1]) + x - rx) != first) {
          return {};
        }
      }
    }
    return convert(first == 0 ? 0 : first, type == ElementType::Float32);
  }
  std::uint64_t refined(Math& m, const Reader& reader, const Coordinate& at,
                        StoredInterval* yweights, StoredInterval* xweights,
                        Visits& visits) const {
    if (const auto same = constant(reader, at, visits)) {
      return *same;
    }
    return m.certify(type == ElementType::Float32, [&](Interval result) {
      auto& d = m.d;
      Directed::Frame frame(d);
      auto zy = d.interval(), zx = d.interval(), denominator = d.interval();
      weights(m, sy, ry, yweights, zy);
      weights(m, sx, rx, xweights, zx);
      d.multiply(denominator, zy, zx);
      auto sum = d.interval(), term = d.interval(), value = d.interval();
      auto weight = d.interval();
      d.integer(sum, 0);
      for (std::uint64_t y = 0; y <= 2 * ry; ++y) {
        for (std::uint64_t x = 0; x <= 2 * rx; ++x) {
          m.work.poll();
          const auto v = sample(reader, __int128(at[0]) + y - ry,
                                __int128(at[1]) + x - rx);
          if (v == 0) {
            continue;
          }
          d.raw(value, bits(v), false);
          d.multiply(weight, yweights[y > ry ? y - ry : ry - y].view(),
                     xweights[x > rx ? x - rx : rx - x].view());
          d.multiply(term, weight, value);
          d.add(sum, sum, term);
        }
      }
      d.divide(result, sum, denominator);
    });
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    const auto& output = inv.outputs[0];
    if (!ry && !rx) {
      samples(output, [&](const Coordinate& at, std::uint64_t) {
        store(output, at, reader.raw(0, 0, at));
        visits.step();
      });
      visits.flush();
      return;
    }
    generation::MathArena arena(inv.scratch[0].allocation, work);
    auto& m = *arena;
    auto& d = m.d;
    const auto address = reinterpret_cast<std::uintptr_t>(arena.value + 1);
    auto* yw =
        reinterpret_cast<StoredInterval*>((address + 15) & ~std::uintptr_t(15));
    auto* xw = yw + ry + 1;
    auto* yf = reinterpret_cast<GaussianBound*>(yw + coefficient_count);
    auto* xf = yf + ry + 1;
    auto* horizontal = yf + coefficient_count;
    d.precision = 128;
    auto zy = d.interval(), zx = d.interval();
    weights(m, sy, ry, yw, zy);
    weights(m, sx, rx, xw, zx);
    fixed_weights(m, yw, ry, zy, yf);
    fixed_weights(m, xw, rx, zx, xf);
    const bool ready = d.active();
    if (d.fault == Directed::Fault::Failure) {
      check(d.status());
    }
    const auto q = invocation_region(*output.layout);
    const auto source_y = source(q).dimensions()[0];
    const auto qx = q.dimensions()[1], qy = q.dimensions()[0];
    if (ready) {
      for (auto y = source_y.offset; y < source_y.offset + source_y.extent;
           ++y) {
        for (auto x = qx.offset; x < qx.offset + qx.extent; ++x) {
          GaussianBound sum;
          for (std::uint64_t tap = 0; tap <= 2 * rx; ++tap) {
            visits.step();
            const auto value =
                gaussian_sample(sample(reader, y, __int128(x) + tap - rx));
            const auto term =
                gaussian_multiply(value, xf[tap > rx ? tap - rx : rx - tap]);
            sum.low += term.low;
            sum.high += term.high;
          }
          sum.high = std::min(sum.high, kGaussianOne);
          horizontal[(y - source_y.offset) * qx.extent + x - qx.offset] = sum;
        }
      }
    }
    const bool narrow = type == ElementType::Float32;
    for (auto y = qy.offset; y < qy.offset + qy.extent; ++y) {
      for (auto x = qx.offset; x < qx.offset + qx.extent; ++x) {
        GaussianBound sum;
        if (ready) {
          for (std::uint64_t tap = 0; tap <= 2 * ry; ++tap) {
            visits.step();
            const auto source_row = map(__int128(y) + tap - ry, height);
            if (source_row < 0) {
              continue;
            }
            const auto value =
                horizontal[(source_row - source_y.offset) * qx.extent + x -
                           qx.offset];
            const auto term =
                gaussian_multiply(value, yf[tap > ry ? tap - ry : ry - tap]);
            sum.low += term.low;
            sum.high += term.high;
          }
        }
        sum.high = std::min(sum.high, kGaussianOne);
        const auto low = gaussian_round(sum.low, narrow),
                   high = gaussian_round(sum.high, narrow);
        const Coordinate at{y, x};
        store(output, at,
              ready && low == high ? low
                                   : refined(m, reader, at, yw, xw, visits));
      }
    }
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_gaussian(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    check(registry->register_operation(
        definition("gaussian_feather", 1,
                   {{"sigma_y", Type::Float64},
                    {"sigma_x", Type::Float64},
                    {"radius_y", Type::Int64},
                    {"radius_x", Type::Int64},
                    {"boundary", Type::String}},
                   [](const auto& in, const auto& p) {
                     return std::make_shared<Gaussian>(in, p);
                   })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
