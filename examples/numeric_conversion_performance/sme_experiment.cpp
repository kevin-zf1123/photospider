#include <arm_neon.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Kernel = std::uint64_t (*)(const std::uint8_t*, std::uint8_t*,
                                 std::uint64_t);
std::uint64_t sme_u8_f32(const std::uint8_t*, std::uint8_t*, std::uint64_t);
std::uint64_t sme_f32_u8(const std::uint8_t*, std::uint8_t*, std::uint64_t);
std::uint64_t sme_f64_f32(const std::uint8_t*, std::uint8_t*, std::uint64_t);
std::uint64_t sme_vector_bytes();
std::uint64_t sme_u8_f32_unrolled(const std::uint8_t*, std::uint8_t*,
                                  std::uint64_t);
std::uint64_t sme_f32_u8_batched(const std::uint8_t*, std::uint8_t*,
                                 std::uint64_t);
std::uint64_t sme_f64_f32_batched(const std::uint8_t*, std::uint8_t*,
                                  std::uint64_t);

std::uint64_t sme_f32_u8_integer_flags(const std::uint8_t*, std::uint8_t*,
                                       std::uint64_t);
std::uint64_t sme_f64_f32_integer_flags(const std::uint8_t*, std::uint8_t*,
                                        std::uint64_t);
std::uint64_t sme_f32_u8_integer_unchecked(const std::uint8_t*, std::uint8_t*,
                                           std::uint64_t);
std::uint64_t sme_f64_f32_unchecked(const std::uint8_t*, std::uint8_t*,
                                    std::uint64_t);
std::uint64_t sme_f32_u8_scheduled(const std::uint8_t*, std::uint8_t*,
                                   std::uint64_t);
std::uint64_t sme_f64_f32_scheduled(const std::uint8_t*, std::uint8_t*,
                                    std::uint64_t);
std::uint64_t sme_f32_u8_span_flags(const std::uint8_t*, std::uint8_t*,
                                    std::uint64_t);
std::uint64_t sme_f64_f32_span_flags(const std::uint8_t*, std::uint8_t*,
                                     std::uint64_t);
std::uint64_t sme_u8_f32_fma(const std::uint8_t*, std::uint8_t*, std::uint64_t);
std::uint64_t sme_f32_u8_throughput(const std::uint8_t*, std::uint8_t*,
                                    std::uint64_t);
std::uint64_t sme_f64_f32_throughput(const std::uint8_t*, std::uint8_t*,
                                     std::uint64_t);
std::uint64_t sme_f32_u8_bits(const std::uint8_t*, std::uint8_t*,
                              std::uint64_t);
std::uint64_t sme_f64_f32_bits(const std::uint8_t*, std::uint8_t*,
                               std::uint64_t);
std::uint64_t sme_f32_u8_bits_unrolled(const std::uint8_t*, std::uint8_t*,
                                       std::uint64_t);
namespace {
// These NEON kernels mirror the committed production span implementations.
std::uint64_t neon_u8_f32(const std::uint8_t* source, std::uint8_t* target,
                          std::uint64_t samples) {
  const auto blocks = samples / 16;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    const auto bytes = vld1q_u8(source + block * 16);
    const auto low = vmovl_u8(vget_low_u8(bytes));
    const auto high = vmovl_u8(vget_high_u8(bytes));
    const uint32x4_t integers[4] = {
        vmovl_u16(vget_low_u16(low)), vmovl_u16(vget_high_u16(low)),
        vmovl_u16(vget_low_u16(high)), vmovl_u16(vget_high_u16(high))};
    for (unsigned i = 0; i < 4; ++i) {
      const auto floats = vcvtq_f32_u32(integers[i]);
      const auto mapped = vdivq_f32(floats, vdupq_n_f32(255.0f));
      std::memcpy(target + block * 64 + i * 16, &mapped, sizeof(mapped));
    }
  }
  return blocks * 16;
}
std::uint64_t neon_f32_u8(const std::uint8_t* source, std::uint8_t* target,
                          std::uint64_t samples) {
  std::uint64_t used = 0;
  while (used + 4 <= samples) {
    float32x4_t values;
    std::memcpy(&values, source + used * 4, 16);
    const auto valid = vandq_u32(vcgeq_f32(values, vdupq_n_f32(0.0f)),
                                 vcleq_f32(values, vdupq_n_f32(1.0f)));
    if (vminvq_u32(valid) != UINT32_MAX)
      break;
    // A binary32 significand times 255 needs at most 32 bits, so the
    // binary64 product is exact before the explicit ties-even conversion.
    const auto lo = vmulq_n_f64(vcvt_f64_f32(vget_low_f32(values)), 255.0);
    const auto hi = vmulq_n_f64(vcvt_f64_f32(vget_high_f32(values)), 255.0);
    std::int64_t rounded[4];
    vst1q_s64(rounded, vcvtnq_s64_f64(lo));
    vst1q_s64(rounded + 2, vcvtnq_s64_f64(hi));
    for (unsigned i = 0; i < 4; ++i)
      target[used + i] = static_cast<std::uint8_t>(rounded[i]);
    used += 4;
  }
  return used;
}
std::uint64_t neon_f64_f32(const std::uint8_t* source, std::uint8_t* target,
                           std::uint64_t samples) {
  std::uint64_t used = 0;
  while (used + 2 <= samples) {
    std::uint64_t bits[2];
    std::memcpy(bits, source + used * 8, sizeof(bits));
    // Keep subnormal results on the bitwise scalar path, independent of
    // the caller's flush-to-zero setting. Nonfinite lanes need payload rules.
    const auto exponent0 = (bits[0] >> 52) & 2047;
    const auto exponent1 = (bits[1] >> 52) & 2047;
    if (exponent0 < 897 || exponent1 < 897 || exponent0 == 2047 ||
        exponent1 == 2047)
      break;
    float64x2_t values;
    std::memcpy(&values, bits, sizeof(values));
    const auto narrowed = vcvt_f32_f64(values);
    const auto result_bits = vreinterpret_u32_f32(narrowed);
    if ((vget_lane_u32(result_bits, 0) & UINT32_C(0x7f800000)) ==
            UINT32_C(0x7f800000) ||
        (vget_lane_u32(result_bits, 1) & UINT32_C(0x7f800000)) ==
            UINT32_C(0x7f800000))
      break;
    std::memcpy(target + used * 4, &narrowed, 8);
    used += 2;
  }
  return used;
}
std::uint64_t sme_f32_u8_host_checked(const std::uint8_t* source,
                                      std::uint8_t* target,
                                      std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto n = std::min<std::uint64_t>(64, count - base);
    auto bad = vdupq_n_u32(0);
    std::uint64_t j = 0;
    for (; j + 4 <= n; j += 4) {
      uint32x4_t words;
      std::memcpy(&words, source + (base + j) * 4, 16);
      bad = vorrq_u32(
          bad, vorrq_u32(words, vsubq_u32(vdupq_n_u32(0x3f800000), words)));
    }
    if (vmaxvq_u32(bad) >> 31)
      return base;
    for (; j < n; ++j) {
      std::uint32_t bits;
      std::memcpy(&bits, source + (base + j) * 4, 4);
      if (bits > 0x3f800000)
        return base;
    }
    sme_f32_u8_integer_unchecked(source + base * 4, target + base, n);
  }
  return count;
}
std::uint64_t sme_f64_f32_host_checked(const std::uint8_t* source,
                                       std::uint8_t* target,
                                       std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto n = std::min<std::uint64_t>(64, count - base);
    auto bad = vdupq_n_u64(0);
    std::uint64_t j = 0;
    for (; j + 2 <= n; j += 2) {
      uint64x2_t words;
      std::memcpy(&words, source + (base + j) * 8, 16);
      const auto mag =
          vandq_u64(words, vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
      bad = vorrq_u64(
          bad,
          vorrq_u64(vsubq_u64(mag, vdupq_n_u64(UINT64_C(0x3810000000000000))),
                    vsubq_u64(vdupq_n_u64(UINT64_C(0x47efffffefffffff)), mag)));
    }
    if ((vgetq_lane_u64(bad, 0) | vgetq_lane_u64(bad, 1)) >> 63)
      return base;
    for (; j < n; ++j) {
      std::uint64_t bits;
      std::memcpy(&bits, source + (base + j) * 8, 8);
      bits &= UINT64_C(0x7fffffffffffffff);
      if (bits < UINT64_C(0x3810000000000000) ||
          bits >= UINT64_C(0x47effffff0000000))
        return base;
    }
    sme_f64_f32_unchecked(source + base * 8, target + base * 4, n);
  }
  return count;
}
std::uint64_t sme_f32_u8_host_span(const std::uint8_t* source,
                                   std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += count) {
    const auto n = count - base;
    auto bad = vdupq_n_u32(0);
    std::uint64_t j = 0;
    for (; j + 4 <= n; j += 4) {
      uint32x4_t words;
      std::memcpy(&words, source + (base + j) * 4, 16);
      bad = vorrq_u32(
          bad, vorrq_u32(words, vsubq_u32(vdupq_n_u32(0x3f800000), words)));
    }
    if (vmaxvq_u32(bad) >> 31)
      return base;
    for (; j < n; ++j) {
      std::uint32_t bits;
      std::memcpy(&bits, source + (base + j) * 4, 4);
      if (bits > 0x3f800000)
        return base;
    }
    sme_f32_u8_integer_unchecked(source + base * 4, target + base, n);
  }
  return count;
}
std::uint64_t sme_f64_f32_host_span(const std::uint8_t* source,
                                    std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += count) {
    const auto n = count - base;
    auto bad = vdupq_n_u64(0);
    std::uint64_t j = 0;
    for (; j + 2 <= n; j += 2) {
      uint64x2_t words;
      std::memcpy(&words, source + (base + j) * 8, 16);
      const auto mag =
          vandq_u64(words, vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
      bad = vorrq_u64(
          bad,
          vorrq_u64(vsubq_u64(mag, vdupq_n_u64(UINT64_C(0x3810000000000000))),
                    vsubq_u64(vdupq_n_u64(UINT64_C(0x47efffffefffffff)), mag)));
    }
    if ((vgetq_lane_u64(bad, 0) | vgetq_lane_u64(bad, 1)) >> 63)
      return base;
    for (; j < n; ++j) {
      std::uint64_t bits;
      std::memcpy(&bits, source + (base + j) * 8, 8);
      bits &= UINT64_C(0x7fffffffffffffff);
      if (bits < UINT64_C(0x3810000000000000) ||
          bits >= UINT64_C(0x47effffff0000000))
        return base;
    }
    sme_f64_f32_unchecked(source + base * 8, target + base * 4, n);
  }
  return count;
}
bool feature(const char* name) {
  int value = 0;
  std::size_t bytes = sizeof(value);
  return sysctlbyname(name, &value, &bytes, nullptr, 0) == 0 && value;
}
void execute(Kernel kernel, const std::vector<std::uint8_t>& source,
             std::vector<std::uint8_t>* target,
             const std::vector<std::uint8_t>& reference,
             std::size_t input_width, std::size_t output_width,
             std::uint64_t block) {
  const auto count = source.size() / input_width;
  for (std::uint64_t i = 0; i < count;) {
    const auto n = std::min<std::uint64_t>(block, count - i);
    const auto converted = kernel(source.data() + i * input_width,
                                  target->data() + i * output_width, n);
    if (converted > n)
      throw std::runtime_error("kernel exceeded admitted span");
    i += converted;
    if (converted != n) {
      // The experiment isolates admitted SIMD spans; exceptional scalar
      // fallback uses precomputed oracle bytes, not production timing.
      std::memcpy(target->data() + i * output_width,
                  reference.data() + i * output_width, output_width);
      ++i;
    }
  }
}
std::uint64_t sme_f32_u8_pipeline(const std::uint8_t* source,
                                  std::uint8_t* target, std::uint64_t count) {
  const auto used = sme_f32_u8_scheduled(source, target, count);
  if (count - used < 64)
    return used + sme_f32_u8(source + used * 4, target + used, count - used);
  return used;
}
std::uint64_t sme_f64_f32_pipeline(const std::uint8_t* source,
                                   std::uint8_t* target, std::uint64_t count) {
  const auto used = sme_f64_f32_scheduled(source, target, count);
  if (count - used < 64)
    return used +
           sme_f64_f32(source + used * 8, target + used * 4, count - used);
  return used;
}
Kernel candidate(unsigned variant, unsigned pair) {
  const Kernel table[10][3] = {
      {sme_u8_f32, sme_f32_u8, sme_f64_f32},
      {sme_u8_f32_unrolled, sme_f32_u8_batched, sme_f64_f32_batched},
      {sme_u8_f32_unrolled, sme_f32_u8_integer_flags,
       sme_f64_f32_integer_flags},
      {sme_u8_f32_unrolled, sme_f32_u8_host_checked, sme_f64_f32_host_checked},
      {sme_u8_f32_unrolled, sme_f32_u8_pipeline, sme_f64_f32_pipeline},
      {sme_u8_f32_fma, sme_f32_u8_span_flags, sme_f64_f32_span_flags},
      {sme_u8_f32_fma, sme_f32_u8_throughput, sme_f64_f32_throughput},
      {sme_u8_f32_fma, sme_f32_u8_bits, sme_f64_f32_bits},
      {sme_u8_f32_fma, sme_f32_u8_host_span, sme_f64_f32_host_span},
      {sme_u8_f32_fma, sme_f32_u8_bits_unrolled, sme_f64_f32_throughput}};
  return table[variant][pair];
}
void verify_candidates(unsigned variant) {
  constexpr std::uint64_t count = 4099;
  const Kernel kernels[] = {candidate(variant, 1), candidate(variant, 2)};
  std::uint64_t random = UINT64_C(0x243f6a8885a308d3);
  for (unsigned pair = 0; pair < 2; ++pair) {
    const std::size_t iw = pair ? 8 : 4, ow = pair ? 4 : 1;
    std::vector<std::uint8_t> source(count * iw), expected(count * ow);
    std::vector<std::uint8_t> actual(count * ow, 0xa5);
    for (std::uint64_t i = 0; i < count; ++i) {
      random = random * UINT64_C(6364136223846793005) + 1;
      if (!pair) {
        const auto bits =
            static_cast<std::uint32_t>(random % UINT32_C(0x3f800001));
        float value;
        std::memcpy(&value, &bits, 4);
        std::memcpy(source.data() + i * 4, &value, 4);
        const double exact = static_cast<double>(value) * 255;
        const auto floor = static_cast<unsigned>(exact);
        const auto fraction = exact - floor;
        expected[i] = static_cast<std::uint8_t>(
            floor + (fraction > 0.5 || (fraction == 0.5 && (floor & 1))));
      } else {
        const auto bits = (random & UINT64_C(0x800fffffffffffff)) |
                          ((897 + ((random >> 12) % 253)) << 52);
        double value;
        std::memcpy(&value, &bits, 8);
        std::memcpy(source.data() + i * 8, &value, 8);
        const float rounded = static_cast<float>(value);
        std::memcpy(expected.data() + i * 4, &rounded, 4);
      }
    }
    if (kernels[pair](source.data(), actual.data(), count) != count ||
        actual != expected)
      throw std::runtime_error("randomized SME oracle mismatch");
    const std::uint64_t exceptional32[] = {
        UINT32_C(0x7fc12345), UINT32_C(0x7f800000), UINT32_C(0xbf800000),
        UINT32_C(0x40000000)};
    const std::uint64_t exceptional64[] = {
        UINT64_C(0x7ff0000000000001), UINT64_C(0x8000000000000000),
        UINT64_C(0x3690000000000000), UINT64_C(0x47f0000000000000)};
    for (auto bits : pair ? exceptional64 : exceptional32) {
      std::memcpy(source.data() + 19 * iw, &bits, iw);
      std::fill(actual.begin(), actual.end(), 0xa5);
      const auto used = kernels[pair](source.data(), actual.data(), count);
      if (used > 19 ||
          !std::equal(actual.begin(), actual.begin() + used * ow,
                      expected.begin()) ||
          !std::all_of(actual.begin() + used * ow, actual.end(),
                       [](std::uint8_t byte) { return byte == 0xa5; }))
        throw std::runtime_error("SME exceptional-lane admission mismatch");
    }
  }
}
void verify_narrowing_boundaries(unsigned variant) {
  const std::uint64_t bounds[] = {UINT64_C(0x3810000000000000),
                                  UINT64_C(0x47effffff0000000)};
  for (auto boundary : bounds)
    for (int offset : {-1, 0, 1})
      for (auto sign : {UINT64_C(0), UINT64_C(0x8000000000000000)}) {
        const auto magnitude = boundary + offset;
        const auto raw = magnitude | sign;
        std::vector<double> input(64, 0.25);
        std::memcpy(&input[17], &raw, 8);
        std::vector<std::uint8_t> output(64 * 4, 0xa5);
        const auto used = candidate(variant, 2)(
            reinterpret_cast<const std::uint8_t*>(input.data()), output.data(),
            input.size());
        const bool admitted = magnitude >= bounds[0] && magnitude < bounds[1];
        if (admitted) {
          if (used != input.size())
            throw std::runtime_error("finite boundary rejected");
          for (unsigned i = 0; i < input.size(); ++i) {
            const float expected = static_cast<float>(input[i]);
            if (std::memcmp(output.data() + i * 4, &expected, 4))
              throw std::runtime_error("boundary bits differ");
          }
        } else if (used > 17 ||
                   !std::all_of(output.begin() + used * 4, output.end(),
                                [](auto x) { return x == 0xa5; })) {
          throw std::runtime_error("unsafe boundary admitted");
        }
      }
}

}  // namespace

int main(int argc, char** argv) try {
  if (!feature("hw.optional.arm.FEAT_SME") ||
      !feature("hw.optional.arm.FEAT_SME_F64F64")) {
    std::cerr << "SME and SME_F64F64 are required\n";
    return 2;
  }
  if (fesetround(FE_TONEAREST) != 0)
    throw std::runtime_error("cannot set nearest rounding");
  const std::uint64_t count = argc > 1 ? std::stoull(argv[1]) : 4194304;
  const unsigned rounds = argc > 2 ? std::stoul(argv[2]) : 7;
  if (count < 259 || count > 67108864 || rounds < 3)
    throw std::runtime_error("count must be 259..67108864; rounds >= 3");
  const std::string selection = argc < 4 ? "throughput" : argv[3];
  const std::vector<std::string> variants{
      "v1",   "batched",    "flags", "host",      "scheduled",
      "span", "throughput", "bits",  "host-span", "unrolled"};
  const auto selected = std::find(variants.begin(), variants.end(), selection);
  if (selected == variants.end())
    throw std::runtime_error("unknown variant");
  const auto variant = static_cast<unsigned>(selected - variants.begin());
  if (sme_vector_bytes() != 64)
    throw std::runtime_error("scheduled experiment currently requires SVL=64");
  verify_candidates(variant);
  verify_narrowing_boundaries(variant);
  std::cout << "variant=" << selection << '\n';
  std::cout << "randomized_and_exceptional_oracles=passed\n";
  std::cout << "streaming_vector_bytes=" << sme_vector_bytes() << '\n';
  std::cout << "pair,block,neon_median_ms,sme_median_ms,speedup\n";
  const Kernel neon[] = {neon_u8_f32, neon_f32_u8, neon_f64_f32};
  const Kernel sme[] = {candidate(variant, 0), candidate(variant, 1),
                        candidate(variant, 2)};
  const char* names[] = {"u8-f32", "f32-u8", "f64-f32"};
  for (unsigned pair = 0; pair < 3; ++pair) {
    const std::size_t iw = pair == 0 ? 1 : pair == 1 ? 4 : 8;
    const std::size_t ow = pair == 1 ? 1 : 4;
    std::vector<std::uint8_t> source(count * iw), reference(count * ow);
    std::vector<std::uint8_t> output(count * ow);
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto code = i % 256;
      if (pair == 0) {
        source[i] = static_cast<std::uint8_t>(code);
        const float expected =
            static_cast<float>(static_cast<double>(code) / 255.0);
        std::memcpy(reference.data() + i * 4, &expected, 4);
      } else if (pair == 1) {
        const float value = static_cast<float>(code) / 255.0f;
        std::memcpy(source.data() + i * 4, &value, 4);
        reference[i] = static_cast<std::uint8_t>(code);
      } else {
        const double value = static_cast<double>(code + 1) / 256.0;
        const float expected = static_cast<float>(value);
        std::memcpy(source.data() + i * 8, &value, 8);
        std::memcpy(reference.data() + i * 4, &expected, 4);
      }
    }
    if (sme[pair](source.data(), output.data(), count) != count)
      throw std::runtime_error(std::string(names[pair]) +
                               " rejected admitted inputs");
    if (output != reference)
      throw std::runtime_error(std::string(names[pair]) +
                               " full-span bit mismatch");
    for (const std::uint64_t block :
         {UINT64_C(64), UINT64_C(128), UINT64_C(4096), UINT64_C(16384),
          UINT64_C(65536)}) {
      std::vector<double> timings[2];
      for (unsigned round = 0; round <= rounds; ++round) {
        for (unsigned step = 0; step < 2; ++step) {
          const unsigned mode = (round + step) % 2;
          const auto start = std::chrono::steady_clock::now();
          execute(mode ? sme[pair] : neon[pair], source, &output, reference, iw,
                  ow, block);
          const auto elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
          if (output != reference)
            throw std::runtime_error(std::string(names[pair]) +
                                     " bit mismatch");
          if (round)
            timings[mode].push_back(elapsed);
        }
      }
      for (auto& values : timings)
        std::sort(values.begin(), values.end());
      const auto n = timings[0][rounds / 2], s = timings[1][rounds / 2];
      std::cout << names[pair] << ',' << block << ',' << n << ',' << s << ','
                << n / s << '\n';
    }
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
