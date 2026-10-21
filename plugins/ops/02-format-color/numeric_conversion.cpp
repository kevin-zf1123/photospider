#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
#include <sys/sysctl.h>
#endif

#include "02-format-color/exact_conversion.hpp"
#include "common/domain/exact_numeric.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
namespace format_numeric {
std::uint64_t sme_conversion_vector_bytes();
std::uint64_t sme_f32_u8_tile(const std::uint8_t*, std::uint8_t*, std::uint64_t,
                              const ps_kernel_cancellation_v8&);
}  // namespace format_numeric
#endif
namespace {
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
using format_numeric::ConversionMap;
using format_numeric::Dyadic;
using format_numeric::ExactConversion;
using Parameters = std::map<std::string, ParameterValue>;
Status invalid(const std::string& detail) {
  return {ErrorCode::InvalidArgument,
          "numeric.convert_format: " + detail,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
bool floating(ElementType t) {
  return t == ElementType::Float32 || t == ElementType::Float64;
}
bool supported(ElementType t) {
  switch (t) {
    case ElementType::UInt8:
    case ElementType::UInt16:
    case ElementType::Int8:
    case ElementType::Int16:
    case ElementType::Int64:
    case ElementType::Float32:
    case ElementType::Float64:
      return true;
  }
  return false;
}
std::size_t sample_width(ElementType t) {
  switch (t) {
    case ElementType::UInt8:
    case ElementType::Int8:
      return 1;
    case ElementType::UInt16:
    case ElementType::Int16:
      return 2;
    case ElementType::Float32:
      return 4;
    case ElementType::Int64:
    case ElementType::Float64:
      return 8;
  }
  return 0;
}
std::optional<ElementType> dtype(const std::string& s) {
  for (const auto& entry : {std::pair{"uint8", ElementType::UInt8},
                            {"uint16", ElementType::UInt16},
                            {"int8", ElementType::Int8},
                            {"int16", ElementType::Int16},
                            {"int64", ElementType::Int64},
                            {"float32", ElementType::Float32},
                            {"float64", ElementType::Float64}})
    if (s == entry.first)
      return entry.second;
  return {};
}
Rational integer_value(std::uint64_t n, bool negative = false) {
  return {Natural(n), Natural(1), negative && n != 0, false};
}
Rational maximum(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
      return integer_value(255);
    case ElementType::UInt16:
      return integer_value(65535);
    case ElementType::Int8:
      return integer_value(127);
    case ElementType::Int16:
      return integer_value(32767);
    case ElementType::Int64:
      return integer_value(INT64_MAX);
    case ElementType::Float32:
    case ElementType::Float64:
      return integer_value(1);
  }
  return integer_value(1);
}
Rational minimum(ElementType type) {
  switch (type) {
    case ElementType::Int8:
      return integer_value(128, true);
    case ElementType::Int16:
      return integer_value(32768, true);
    case ElementType::Int64:
      return integer_value(UINT64_C(1) << 63, true);
    default:
      return integer_value(0);
  }
}
// Range text is a static typed codec: i:<signed decimal> or f64:<16 hex
// binary64 bits>, two endpoints separated by comma, channels by semicolon.
// Parsing never rounds through a machine floating conversion.
Result<Rational> token(const std::string& value) {
  if (value.size() < 3)
    return Result<Rational>(invalid("malformed endpoint"));
  if (value.compare(0, 2, "i:") == 0) {
    std::size_t at = 2;
    bool negative = false;
    if (at < value.size() && value[at] == '-') {
      negative = true;
      ++at;
    }
    if (at == value.size())
      return Result<Rational>(invalid("empty integer endpoint"));
    std::uint64_t n = 0;
    for (; at < value.size(); ++at) {
      if (value[at] < '0' || value[at] > '9')
        return Result<Rational>(invalid("invalid integer endpoint"));
      const unsigned digit = value[at] - '0';
      if (n > (UINT64_MAX - digit) / 10)
        return Result<Rational>(invalid("integer endpoint exceeds 64 bits"));
      n = n * 10 + digit;
    }
    return Result<Rational>(integer_value(n, negative));
  }
  if (value.compare(0, 4, "f64:") == 0 && value.size() == 20) {
    std::uint64_t bits = 0;
    for (std::size_t i = 4; i < value.size(); ++i) {
      const char c = value[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return Result<Rational>(invalid("invalid binary64 endpoint bits"));
      bits = (bits << 4) |
             static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return Result<Rational>(Rational::binary(bits, false));
  }
  return Result<Rational>(invalid("endpoint must use i: or f64: typed syntax"));
}
struct Bounds final {
  Rational source_lo, source_hi, target_lo, target_hi;
  bool identity = false;
};
Result<std::vector<std::array<Rational, 2>>> range(const std::string& text) {
  using Output = Result<std::vector<std::array<Rational, 2>>>;
  std::vector<std::array<Rational, 2>> entries;
  std::size_t start = 0;
  for (;;) {
    const auto end = text.find(';', start);
    const auto pair =
        text.substr(start, end == std::string::npos ? end : end - start);
    const auto comma = pair.find(',');
    if (comma == std::string::npos ||
        pair.find(',', comma + 1) != std::string::npos)
      return Output(invalid("each range entry requires two endpoints"));
    auto lo = token(pair.substr(0, comma));
    if (!lo.ok())
      return Output(lo.status());
    auto hi = token(pair.substr(comma + 1));
    if (!hi.ok())
      return Output(hi.status());
    entries.push_back({lo.take_value(), hi.take_value()});
    if (entries.size() > 4096)
      return Output(invalid("range table too large"));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return Output(std::move(entries));
}
struct FixedBounds final {
  ConversionMap map;
  bool identity = false;
};
struct RuntimeConversion {
  ElementType source, target;
  bool rescale, clip, identity;
  bool sme = false;
  std::string layout;
  std::optional<std::uint32_t> sample_axis;
  bool fast_u8_f32 = false;
  bool fast_f32_u8 = false;
  bool fast_i64_u8 = false;
  bool fast_f64_f32 = false;
  std::shared_ptr<const std::vector<FixedBounds>> fixed_bounds;
  std::array<std::uint32_t, 256> u8_f32{};
};
struct Preparation final : RuntimeConversion {
  ResourceVector<Bounds> bounds;
  std::optional<std::uint32_t> axis;
};
Dyadic dyadic(const Rational& value) {
  auto magnitude = value.n;
  const auto zeros = magnitude.trailing_zeros();
  magnitude.shift_right(zeros);
  return Dyadic::normalized(
      magnitude.low64(),
      static_cast<int>(zeros) - static_cast<int>(value.d.bits() - 1),
      value.negative || value.negative_zero);
}
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Parameters& params) {
  using Output = Result<OperationPreparation>;
  if (inputs.size() != 1)
    return Output(invalid("one input is required"));
  auto checked = tensor_ops::check_tensor(inputs[0]);
  if (!checked.ok())
    return Output(checked);
  const auto& input = inputs[0].result_schema->tensors[0];
  const auto& shape = input.descriptor.shape;
  const auto full_shape = input.sample_shape();
  if (!supported(input.descriptor.element_type) || shape.empty() ||
      full_shape.size() > 8)
    return Output(Status{ErrorCode::TypeMismatch,
                         "numeric.convert_format requires rank 1..8"});
  std::uint64_t elements = 1;
  for (const auto extent : full_shape) {
    if (!extent || extent > ((UINT64_C(1) << 40) / elements))
      return Output(Status{ErrorCode::TypeMismatch,
                           "numeric.convert_format exceeds 2^40 elements"});
    elements *= extent;
  }
  auto target = dtype(std::get<std::string>(params.at("dtype")));
  if (!target)
    return Output(invalid("unsupported dtype"));
  auto state = std::make_shared<Preparation>();
  state->source = input.descriptor.element_type;
  state->target = *target;
  state->rescale =
      params.count("rescale") ? std::get<bool>(params.at("rescale")) : true;
  const auto rounding = params.count("rounding")
                            ? std::get<std::string>(params.at("rounding"))
                            : "ties_even";
  if (rounding != "ties_even")
    return Output(invalid("unsupported rounding"));
  const auto overflow = params.count("overflow")
                            ? std::get<std::string>(params.at("overflow"))
                            : "reject";
  if (overflow != "reject" && overflow != "clip")
    return Output(invalid("overflow must be reject or clip"));
  state->clip = overflow == "clip";
  const auto layout = params.count("layout")
                          ? std::get<std::string>(params.at("layout"))
                          : "auto";
  if (layout != "auto" && layout != "view" && layout != "materialize")
    return Output(invalid("layout must be auto, view or materialize"));
  state->layout = layout;
  const bool source_explicit = params.count("source_range");
  const bool target_explicit = params.count("target_range");
  if (!state->rescale &&
      (source_explicit || target_explicit || params.count("axis")))
    return Output(invalid("pure cast forbids range and axis parameters"));
  std::vector<std::array<Rational, 2>> source, destination;
  if (source_explicit) {
    auto parsed = range(std::get<std::string>(params.at("source_range")));
    if (!parsed.ok())
      return Output(parsed.status());
    source = parsed.take_value();
  } else {
    source.push_back({minimum(state->source), maximum(state->source)});
  }
  if (target_explicit) {
    auto parsed = range(std::get<std::string>(params.at("target_range")));
    if (!parsed.ok())
      return Output(parsed.status());
    destination = parsed.take_value();
  } else {
    destination.push_back({minimum(state->target), maximum(state->target)});
  }
  const bool tabulated = source.size() > 1 || destination.size() > 1;
  if (tabulated != static_cast<bool>(params.count("axis")))
    return Output(invalid("axis is required exactly for channel tables"));
  if (tabulated) {
    const auto a = std::get<std::int64_t>(params.at("axis"));
    if (a < 0 || static_cast<std::uint64_t>(a) >= shape.size())
      return Output(invalid("channel axis outside rank"));
    state->axis = static_cast<std::uint32_t>(a);
    state->sample_axis =
        static_cast<std::uint32_t>(input.batch_axes.size() + a);
    const auto count = shape[*state->axis];
    if ((source.size() != 1 && source.size() != count) ||
        (destination.size() != 1 && destination.size() != count))
      return Output(invalid("range table must cover every channel"));
  }
  const std::size_t entries = std::max(source.size(), destination.size());
  state->identity = state->source == state->target;
  for (std::size_t i = 0; i < entries; ++i) {
    const auto& s = source[source.size() == 1 ? 0 : i];
    const auto& t = destination[destination.size() == 1 ? 0 : i];
    Bounds b{s[0], s[1], t[0], t[1], false};
    b.identity = !state->rescale ||
                 (s[0].compare(t[0]) == 0 && s[1].compare(t[1]) == 0 &&
                  s[0].negative_zero == t[0].negative_zero &&
                  s[1].negative_zero == t[1].negative_zero);
    state->identity &= b.identity;
    state->bounds.push_back(std::move(b));
  }
  state->fast_u8_f32 =
      state->source == ElementType::UInt8 &&
      state->target == ElementType::Float32 && state->rescale &&
      source.size() == 1 && destination.size() == 1 &&
      state->bounds[0].source_lo.compare(integer_value(0)) == 0 &&
      state->bounds[0].source_hi.compare(integer_value(255)) == 0 &&
      state->bounds[0].target_lo.compare(integer_value(0)) == 0 &&
      !state->bounds[0].target_lo.negative_zero &&
      state->bounds[0].target_hi.compare(integer_value(1)) == 0;
  state->fast_f32_u8 =
      state->source == ElementType::Float32 &&
      state->target == ElementType::UInt8 && state->rescale &&
      source.size() == 1 && destination.size() == 1 &&
      state->bounds[0].source_lo.compare(integer_value(0)) == 0 &&
      state->bounds[0].source_hi.compare(integer_value(1)) == 0 &&
      state->bounds[0].target_lo.compare(integer_value(0)) == 0 &&
      state->bounds[0].target_hi.compare(integer_value(255)) == 0;
  state->fast_i64_u8 =
      state->source == ElementType::Int64 &&
      state->target == ElementType::UInt8 && state->rescale &&
      source.size() == 1 && destination.size() == 1 &&
      state->bounds[0].source_lo.compare(minimum(ElementType::Int64)) == 0 &&
      state->bounds[0].source_hi.compare(maximum(ElementType::Int64)) == 0 &&
      state->bounds[0].target_lo.compare(integer_value(0)) == 0 &&
      state->bounds[0].target_hi.compare(integer_value(255)) == 0;
  state->fast_f64_f32 = state->source == ElementType::Float64 &&
                        state->target == ElementType::Float32 &&
                        (!state->rescale || (state->bounds.size() == 1 &&
                                             state->bounds[0].identity));
  if (layout == "view" && !state->identity)
    return Output(invalid("ViewUnavailable: mapping is not an identity"));

  auto maps = std::make_shared<std::vector<FixedBounds>>();
  maps->reserve(state->bounds.size());
  ExactConversion math;
  const auto consume = [](std::uint64_t work) {
    Natural::work(work);
    return Status::success();
  };
  for (const auto& bounds : state->bounds) {
    FixedBounds fixed;
    fixed.identity = bounds.identity;
    fixed.map.source[0] = dyadic(bounds.source_lo);
    fixed.map.source[1] = dyadic(bounds.source_hi);
    fixed.map.target[0] = dyadic(bounds.target_lo);
    fixed.map.target[1] = dyadic(bounds.target_hi);
    auto status = math.initialize(&fixed.map, consume);
    if (!status.ok())
      return Output(status);
    maps->push_back(std::move(fixed));
  }
  state->fixed_bounds = std::move(maps);
  if (state->fast_u8_f32) {
    for (unsigned i = 0; i < state->u8_f32.size(); ++i)
      state->u8_f32[i] = static_cast<std::uint32_t>(
          Rational::divide(integer_value(i), integer_value(255))
              .floating_bits(true));
  }
  OperationOutputSpecialization output;
  auto schema = *inputs[0].result_schema;
  auto& output_tensor = schema.tensors[0];
  output_tensor.descriptor.element_type = state->target;
  output_tensor.atomic_trailing_axes = 0;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(state);
  return Output(std::move(result));
}

void write_raw(std::uint8_t* target, ElementType type, std::uint64_t bits) {
  std::memcpy(target, &bits, sample_width(type));
}
std::uint64_t nan_bits(std::uint64_t input, ElementType source,
                       ElementType destination) {
  const unsigned from = source == ElementType::Float32 ? 23 : 52;
  const unsigned to = destination == ElementType::Float32 ? 23 : 52;
  const std::uint64_t sign = input >> (from + (from == 23 ? 8 : 11));
  std::uint64_t payload =
      input & ((static_cast<std::uint64_t>(1) << (from - 1)) - 1);
  if (to > from)
    payload <<= to - from;
  else
    payload >>= from - to;
  return (sign << (to + (to == 23 ? 8 : 11))) |
         (static_cast<std::uint64_t>(to == 23 ? 255 : 2047) << to) |
         (static_cast<std::uint64_t>(1) << (to - 1)) | payload;
}
std::uint32_t narrow_f64_bits(std::uint64_t bits) {
  const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 63) << 31;
  const auto exponent = static_cast<int>((bits >> 52) & 2047);
  const std::uint64_t fraction = bits & UINT64_C(0xfffffffffffff);
  if (exponent == 2047)
    return sign |
           (fraction ? static_cast<std::uint32_t>(nan_bits(
                           bits, ElementType::Float64, ElementType::Float32)) &
                           UINT32_C(0x7fffffff)
                     : UINT32_C(0x7f800000));
  if (!exponent && !fraction)
    return sign;
  // Every binary64 subnormal is far below half a binary32 subnormal.
  if (!exponent)
    return sign;
  int e = exponent - 1023;
  if (e > 127)
    return sign | UINT32_C(0x7f800000);
  const std::uint64_t significand = fraction | (UINT64_C(1) << 52);
  int shift = 29 + std::max(0, -126 - e);
  if (shift >= 64)
    return sign;
  std::uint64_t rounded = significand >> shift;
  const std::uint64_t remainder = significand & ((UINT64_C(1) << shift) - 1);
  const std::uint64_t half = UINT64_C(1) << (shift - 1);
  if (remainder > half || (remainder == half && (rounded & 1)))
    ++rounded;
  if (e < -126) {
    if (rounded >= (UINT64_C(1) << 23))
      return sign | UINT32_C(0x00800000);
    return sign | static_cast<std::uint32_t>(rounded);
  }
  if (rounded == (UINT64_C(1) << 24)) {
    rounded >>= 1;
    ++e;
  }
  if (e > 127)
    return sign | UINT32_C(0x7f800000);
  return sign | (static_cast<std::uint32_t>(e + 127) << 23) |
         (static_cast<std::uint32_t>(rounded) & UINT32_C(0x007fffff));
}
std::uint8_t mapped_i64_u8(std::uint64_t input) {
  // D = 2^64 - 1 is odd, so rounding p/D has no half ties. After adding
  // floor(D/2), divide high*2^64 + low by D as high + (high + low >= D).
  const auto offset = input - (UINT64_C(1) << 63);
  const auto numerator =
      static_cast<unsigned __int128>(offset) * 255 + (UINT64_MAX >> 1);
  const auto high = static_cast<std::uint64_t>(numerator >> 64);
  const auto low = static_cast<std::uint64_t>(numerator);
  return static_cast<std::uint8_t>(high + (low >= UINT64_MAX - high));
}
void i64_u8_span(const std::uint8_t* source, std::uint8_t* target,
                 std::uint64_t samples) {
  for (std::uint64_t i = 0; i < samples; ++i) {
    std::uint64_t bits;
    std::memcpy(&bits, source + i * 8, 8);
    target[i] = mapped_i64_u8(bits);
  }
}
Result<std::uint64_t> convert_one(std::uint64_t input,
                                  const RuntimeConversion& state,
                                  const FixedBounds& bounds,
                                  ExactConversion* math,
                                  const core_internal::WorkConsumer& consume) {
  using Answer = Result<std::uint64_t>;
  if (state.source == state.target && bounds.identity)
    return Answer(input);
  if (state.fast_u8_f32)
    return Answer(state.u8_f32[input]);
  if (state.fast_i64_u8)
    return Answer(mapped_i64_u8(input));
  Dyadic value;
  if (floating(state.source)) {
    const bool narrow = state.source == ElementType::Float32;
    const auto parts = numeric_ops::BinaryParts::decode(input, narrow);
    if (floating(state.target) && (parts.nan || parts.infinite)) {
      if (parts.nan)
        return Answer(nan_bits(input, state.source, state.target));
      const bool sign =
          parts.negative != (state.rescale && bounds.map.slope_negative);
      const bool small = state.target == ElementType::Float32;
      return Answer(
          (static_cast<std::uint64_t>(sign) << (small ? 31 : 63)) |
          (small ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)));
    }
    value = Dyadic::binary(input, narrow);
    if (state.fast_f64_f32 && (!state.rescale || parts.magnitude)) {
      auto bits = narrow_f64_bits(input);
      if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) && state.clip)
        bits = (bits & UINT32_C(0x80000000)) | UINT32_C(0x7f7fffff);
      return Answer(bits);
    }
  } else {
    bool sign = false;
    switch (state.source) {
      case ElementType::Int8:
        sign = (input & 128) != 0;
        if (sign)
          input = 256 - input;
        break;
      case ElementType::Int16:
        sign = (input & 32768) != 0;
        if (sign)
          input = 65536 - input;
        break;
      case ElementType::Int64:
        sign = (input >> 63) != 0;
        if (sign)
          input = UINT64_C(0) - input;
        break;
      default:
        break;
    }
    value = Dyadic::normalized(input, 0, sign);
  }
  auto transformed =
      math->map_value(value, state.rescale ? &bounds.map : nullptr, consume);
  if (!transformed.ok())
    return Answer(transformed);
  if (floating(state.target))
    return math->floating(state.target == ElementType::Float32, state.clip,
                          consume);
  std::uint64_t positive = 0, negative = 0;
  switch (state.target) {
    case ElementType::UInt8:
      positive = 255;
      break;
    case ElementType::UInt16:
      positive = 65535;
      break;
    case ElementType::Int8:
      positive = 127;
      negative = 128;
      break;
    case ElementType::Int16:
      positive = 32767;
      negative = 32768;
      break;
    case ElementType::Int64:
      positive = INT64_MAX;
      negative = UINT64_C(1) << 63;
      break;
    default:
      break;
  }
  return math->integer(positive, negative, state.clip, consume);
}

#if defined(__aarch64__)
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
#endif

#if defined(__x86_64__)
__attribute__((target("avx2"))) std::uint64_t avx2_u8_f32(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t samples) {
  const auto blocks = samples / 8;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    std::uint64_t bytes;
    std::memcpy(&bytes, source + block * 8, sizeof(bytes));
    const auto integers = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(bytes));
    const auto floats = _mm256_cvtepi32_ps(integers);
    const auto mapped = _mm256_div_ps(floats, _mm256_set1_ps(255.0f));
    std::memcpy(target + block * 32, &mapped, sizeof(mapped));
  }
  return blocks * 8;
}
__attribute__((target("avx2"))) std::uint64_t avx2_f32_u8(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t samples) {
  std::uint64_t used = 0;
  while (used + 4 <= samples) {
    __m128 values;
    std::memcpy(&values, source + used * 4, sizeof(values));
    const auto valid = _mm_and_ps(_mm_cmpge_ps(values, _mm_setzero_ps()),
                                  _mm_cmple_ps(values, _mm_set1_ps(1.0f)));
    if (_mm_movemask_ps(valid) != 15)
      break;
    // As in NEON, widening makes multiplication by 255 exact.
    const auto mapped =
        _mm256_mul_pd(_mm256_cvtps_pd(values), _mm256_set1_pd(255.0));
    const auto rounded =
        _mm256_round_pd(mapped, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    const auto integers = _mm256_cvttpd_epi32(rounded);
    std::int32_t lanes[4];
    std::memcpy(lanes, &integers, sizeof(lanes));
    for (unsigned i = 0; i < 4; ++i)
      target[used + i] = static_cast<std::uint8_t>(lanes[i]);
    used += 4;
  }
  return used;
}
__attribute__((target("avx2"))) std::uint64_t avx2_f64_f32(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t samples) {
  std::uint64_t used = 0;
  while (used + 4 <= samples) {
    __m256i bits;
    std::memcpy(&bits, source + used * 8, sizeof(bits));
    const auto exponent =
        _mm256_and_si256(_mm256_srli_epi64(bits, 52), _mm256_set1_epi64x(2047));
    // Scalar bit conversion handles zero, subnormals (including FTZ/DAZ),
    // nonfinite payloads and endpoint semantics.
    const auto normal = _mm256_and_si256(
        _mm256_cmpgt_epi64(exponent, _mm256_set1_epi64x(896)),
        _mm256_cmpgt_epi64(_mm256_set1_epi64x(2047), exponent));
    if (_mm256_movemask_epi8(normal) != -1)
      break;
    const auto narrowed = _mm256_cvtpd_ps(_mm256_castsi256_pd(bits));
    const auto result_exponent =
        _mm_and_si128(_mm_castps_si128(narrowed), _mm_set1_epi32(0x7f800000));
    if (_mm_movemask_epi8(
            _mm_cmpeq_epi32(result_exponent, _mm_set1_epi32(0x7f800000))) != 0)
      break;
    std::memcpy(target + used * 4, &narrowed, sizeof(narrowed));
    used += 4;
  }
  return used;
}
#endif

// Returns only the successfully converted prefix; the caller handles the
// first unsupported lane with convert_one and may resume SIMD afterwards.
std::uint64_t convert_span(const std::uint8_t* source, std::uint8_t* target,
                           std::uint64_t count,
                           const RuntimeConversion& state) {
  if (state.identity) {
    std::memcpy(target, source, count * sample_width(state.source));
    return count;
  }
  if (state.fast_i64_u8) {
    i64_u8_span(source, target, count);
    return count;
  }
#if defined(__aarch64__)
  if (state.fast_u8_f32)
    return neon_u8_f32(source, target, count);
  if (state.fast_f32_u8)
    return neon_f32_u8(source, target, count);
  if (state.fast_f64_f32)
    return neon_f64_f32(source, target, count);
#elif defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) {
    if (state.fast_u8_f32)
      return avx2_u8_f32(source, target, count);
    if (state.fast_f32_u8)
      return avx2_f32_u8(source, target, count);
    if (state.fast_f64_f32)
      return avx2_f64_f32(source, target, count);
  }
#endif
  return 0;
}

constexpr const char* conversion_contract = "numeric-conversion-unchecked-v2";
std::uint64_t scratch_bytes(const Preparation& state) {
  return state.layout == "view" || state.identity ? 0 : sizeof(ExactConversion);
}
Result<OperationProgram> lower(const OperationLowering& query) {
  const auto& state = *static_cast<const Preparation*>(query.static_state);
  if (state.layout != "view")
    return fixed_tensor::lower_pointwise(query, "convert", conversion_contract,
                                         scratch_bytes(state));
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("convert", conversion_contract);
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      auto status = builder.read_input_schema(task.value(), 0);
      if (status.ok())
        status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
  return builder.finish();
}
struct PreparedConversion final {
  RuntimeConversion state;
  fixed_tensor::PointInputs<1> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& prepared =
      *static_cast<const PreparedConversion*>(invocation->prepared_context);
  const auto& state = prepared.state;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& checked) {
    if (!checked.ok())
      *status =
          kernel_failure(checked.code, checked.message.c_str(), checked.reason);
    return checked.ok();
  };
  if (!accept(consume(0)))
    return;
  if (state.layout == "view") {
    *status = kernel_failure(ErrorCode::InvalidArgument, "ViewUnavailable",
                             FailureReason::ViewUnavailable);
    return;
  }
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  ExactConversion* math =
      state.identity ? nullptr
                     : new (invocation->scratch[0].allocation) ExactConversion;
  fixed_tensor::PointReader<1> reader{prepared.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto rank = output.layout->rank;
  const auto source_width = sample_width(state.source),
             target_width = sample_width(state.target);
  auto* target =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  std::array<std::uint64_t, 8> at{};
  std::uint64_t samples = 1;
  for (unsigned axis = 0; axis < rank; ++axis) {
    at[axis] = output.layout->origin[axis];
    samples *= output.layout->shape[axis];
  }
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
  if (state.sme && samples >= 4096 && samples <= 65536) {
    reader.bits(0, at, source_width);
    const auto& view = *reader.cached[0];
    bool contiguous = view.layout->rank == rank;
    std::uint64_t stride = source_width;
    for (unsigned axis = rank; axis-- > 0;) {
      contiguous &=
          view.layout->origin[axis] == output.layout->origin[axis] &&
          view.layout->shape[axis] == output.layout->shape[axis] &&
          view.layout->strides[axis] == static_cast<std::int64_t>(stride);
      stride *= output.layout->shape[axis];
    }
    const auto* source =
        static_cast<const std::uint8_t*>(view.allocation) + view.byte_offset;
    if (contiguous && reinterpret_cast<std::uintptr_t>(source) % 4 == 0) {
      const auto used = format_numeric::sme_f32_u8_tile(
          source, target, samples, invocation->cancellation);
      if (!accept(consume(0)))
        return;
      if (used == samples) {
        *status = {};
        return;
      }
    }
  }
#endif
  for (std::uint64_t sample = 0; sample < samples;) {
    if (!accept(consume(0)))
      return;
    const auto input = reader.bits(0, at, source_width);
    const auto& view = *reader.cached[0];
    std::uint64_t used = 0;
    if (view.layout->strides[rank - 1] ==
        static_cast<std::int64_t>(source_width)) {
      const auto count = std::min<std::uint64_t>(
          {64,
           view.layout->origin[rank - 1] + view.layout->shape[rank - 1] -
               at[rank - 1],
           output.layout->origin[rank - 1] + output.layout->shape[rank - 1] -
               at[rank - 1]});
      __int128 offset = view.byte_offset;
      for (unsigned axis = 0; axis < rank; ++axis)
        offset += static_cast<__int128>(at[axis] - view.layout->origin[axis]) *
                  view.layout->strides[axis];
      used = convert_span(static_cast<const std::uint8_t*>(view.allocation) +
                              static_cast<std::ptrdiff_t>(offset),
                          target + sample * target_width, count, state);
    }
    if (!used) {
      const auto& bounds =
          (*state.fixed_bounds)[state.sample_axis ? at[*state.sample_axis] : 0];
      auto converted = convert_one(input, state, bounds, math, consume);
      if (!accept(converted.status()))
        return;
      write_raw(target + sample * target_width, state.target,
                converted.value());
      used = 1;
    }
    sample += used;
    at[rank - 1] += used;
    for (unsigned axis = rank; axis-- > 0;) {
      if (at[axis] < output.layout->origin[axis] + output.layout->shape[axis])
        break;
      at[axis] = output.layout->origin[axis];
      if (axis)
        ++at[axis - 1];
    }
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(Status{ErrorCode::TypeMismatch,
                         "invalid numeric conversion signature"});
  };
  const auto* state = static_cast<const Preparation*>(query.static_state);
  if (!state || query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(*state), 0, 0})
    return invalid();
  auto expected = *query.inputs[0].result_schema;
  expected.tensors[0].descriptor.element_type = state->target;
  expected.tensors[0].atomic_trailing_axes = 0;
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(expected))
    return invalid();
  auto prepared = std::make_unique<PreparedConversion>();
  prepared->state = static_cast<const RuntimeConversion&>(*state);
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
  if (state->fast_f32_u8) {
    bool available = true;
    for (const auto* name :
         {"hw.optional.arm.FEAT_SME", "hw.optional.arm.FEAT_SME_F64F64"}) {
      int value = 0;
      std::size_t bytes = sizeof(value);
      available &= sysctlbyname(name, &value, &bytes, nullptr, 0) == 0 && value;
    }
    prepared->state.sme =
        available && format_numeric::sme_conversion_vector_bytes() == 64;
  }
#endif
  auto grid = TileGrid::from_schema(expected, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(query.writes[0].tile_index);
  if (!region.ok())
    return Answer(region.status());
  std::array<Region, 1> regions{
      state->layout == "view"
          ? Region(std::vector<RegionDimension>(region.value().rank()))
          : region.value()};
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &prepared->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedConversion*>(context);
  };
  return Answer(kernel);
}

OperationDefinition conversion() {
  OperationDefinition definition;
  definition.key = "numeric.convert_format_strict";
  auto& traits = definition.traits;
  traits.input_count = 1;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  traits.input_schema = {port};
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"dtype", OperationParameterType::String},
      {"rescale", OperationParameterType::Bool, false},
      {"source_range", OperationParameterType::String, false},
      {"target_range", OperationParameterType::String, false},
      {"axis", OperationParameterType::Int64, false},
      {"rounding", OperationParameterType::String, false},
      {"overflow", OperationParameterType::String, false},
      {"layout", OperationParameterType::String, false}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.output_schema = port;
  output.result_schema = tensor_ops::scalar_schema();
  definition.prepare_static = [](const auto& inputs, const auto& params) {
    return prepare(inputs, params);
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "convert" || kernel.contract != conversion_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid conversion kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "conversion requires CPU Host"});
    return prepare_kernel(query);
  };
  return definition;
}
}  // namespace

Status register_numeric_conversion(OperationRegistry* registry) {
  return registry->register_operation(conversion());
}
}  // namespace ps::plugin_internal
