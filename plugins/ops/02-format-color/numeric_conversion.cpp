#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
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

#include "execution/cancellation_poll.hpp"
#endif

#include "01-numeric/array_publication.hpp"
#include "data/exact_numeric.hpp"
#include "photospider/data/tensor_description.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
namespace format_numeric {
std::uint64_t sme_conversion_vector_bytes();
std::uint64_t sme_f32_u8_tile(const std::uint8_t*, std::uint8_t*, std::uint64_t,
                              const execution_internal::CancellationPoll&);
}  // namespace format_numeric
#endif
namespace {
using data_internal::format_numeric::ExactWorkFailure;
using data_internal::format_numeric::ExactWorkScope;
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
using Parameters = std::map<std::string, ParameterValue>;
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
bool sme_conversion_available() {
  static const bool available = [] {
    for (const auto* name :
         {"hw.optional.arm.FEAT_SME", "hw.optional.arm.FEAT_SME_F64F64"}) {
      int value = 0;
      std::size_t bytes = sizeof(value);
      if (sysctlbyname(name, &value, &bytes, nullptr, 0) != 0 || !value)
        return false;
    }
    return format_numeric::sme_conversion_vector_bytes() == 64;
  }();
  return available;
}
#endif
struct FloatingEnvironment final {
  fenv_t saved{};
  FloatingEnvironment() {
    fegetenv(&saved);
    fesetround(FE_TONEAREST);
    feclearexcept(FE_ALL_EXCEPT);
  }
  ~FloatingEnvironment() { fesetenv(&saved); }
};

Status invalid(const std::string& detail) {
  return {ErrorCode::InvalidArgument,
          "numeric.convert_format: " + detail,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
Status sample_error(const std::string& detail, FailureReason reason) {
  return {ErrorCode::OperationFailed,
          "numeric.convert_format: " + detail,
          reason,
          {FailureOrigin::Domain, FailureScope::Unspecified}};
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
  return {Natural(n), Natural(1), negative, false};
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
std::uint64_t bits64(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, 8);
  return bits;
}
Rational endpoint(const TensorEndpoint& value) {
  if (const auto* i = std::get_if<std::int64_t>(&value))
    return Rational::integer(*i);
  if (const auto* f = std::get_if<double>(&value))
    return Rational::binary(bits64(*f), false);
  const auto& exact = std::get<TensorRationalEndpoint>(value);
  Rational result;
  result.n.words.assign(exact.numerator.begin(), exact.numerator.end());
  result.d.words.assign(exact.denominator.begin(), exact.denominator.end());
  result.n.trim();
  result.d.trim();
  result.negative = exact.negative;
  return result;
}
Result<TensorEndpoint> represented(const Rational& value) {
  const auto signed_limit = Rational::integer(INT64_MIN);
  const auto upper_limit = Rational::integer(INT64_MAX);
  if (value.d.compare(Natural(1)) == 0 && value.compare(signed_limit) >= 0 &&
      value.compare(upper_limit) <= 0 && !value.negative_zero) {
    const auto magnitude = value.n.low64();
    return Result<TensorEndpoint>(TensorEndpoint{static_cast<std::int64_t>(
        value.negative ? UINT64_C(0) - magnitude : magnitude)});
  }
  const auto bits = value.floating_bits(false);
  if (((bits >> 52) & 2047) != 2047 &&
      Rational::binary(bits, false).compare(value) == 0) {
    double sample;
    std::memcpy(&sample, &bits, sizeof(sample));
    return Result<TensorEndpoint>(TensorEndpoint{sample});
  }
  Natural numerator = value.n, denominator = value.d;
  const Natural divisor = Natural::gcd(numerator, denominator);
  numerator = Natural::divide(numerator, divisor).first;
  denominator = Natural::divide(denominator, divisor).first;
  if (numerator.words.size() > 128 || denominator.words.size() > 128)
    return Result<TensorEndpoint>(
        invalid("exact decoder endpoint exceeds rational metadata limit"));
  TensorRationalEndpoint exact;
  exact.numerator.assign(numerator.words.begin(), numerator.words.end());
  exact.denominator.assign(denominator.words.begin(), denominator.words.end());
  exact.negative = value.negative;
  return Result<TensorEndpoint>(TensorEndpoint{std::move(exact)});
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
    if (((bits >> 52) & 2047) == 2047)
      return Result<Rational>(invalid("nonfinite endpoint"));
    return Result<Rational>(Rational::binary(bits, false));
  }
  return Result<Rational>(invalid("endpoint must use i: or f64: typed syntax"));
}
struct Bounds final {
  Rational source_lo, source_hi, target_lo, target_hi;
  bool identity = false;
};
Result<TensorEncoding> mapped_encoding(
    const std::optional<TensorEncoding>& prior, const Bounds& b, bool rescale) {
  using Output = Result<TensorEncoding>;
  if (!rescale) {
    if (prior)
      return Output(*prior);
    return Output(TensorEncoding{});
  }
  const Rational old_lo = prior ? endpoint(prior->stored[0]) : b.source_lo;
  const Rational old_hi = prior ? endpoint(prior->stored[1]) : b.source_hi;
  const Rational decoded_lo = prior ? endpoint(prior->decoded[0]) : b.source_lo;
  const Rational decoded_hi = prior ? endpoint(prior->decoded[1]) : b.source_hi;
  const auto decode = [&](const Rational& source) {
    return Rational::add(
        decoded_lo,
        Rational::multiply(Rational::divide(Rational::subtract(source, old_lo),
                                            Rational::subtract(old_hi, old_lo)),
                           Rational::subtract(decoded_hi, decoded_lo)));
  };
  auto stored_lo = represented(b.target_lo);
  if (!stored_lo.ok())
    return Output(stored_lo.status());
  auto stored_hi = represented(b.target_hi);
  if (!stored_hi.ok())
    return Output(stored_hi.status());
  auto mapped_lo = represented(prior ? decode(b.source_lo) : b.source_lo);
  if (!mapped_lo.ok())
    return Output(mapped_lo.status());
  auto mapped_hi = represented(prior ? decode(b.source_hi) : b.source_hi);
  if (!mapped_hi.ok())
    return Output(mapped_hi.status());
  TensorEncoding result;
  if (b.target_lo.compare(b.target_hi) < 0) {
    result.stored = {stored_lo.take_value(), stored_hi.take_value()};
    result.decoded = {mapped_lo.take_value(), mapped_hi.take_value()};
  } else {
    result.stored = {stored_hi.take_value(), stored_lo.take_value()};
    result.decoded = {mapped_hi.take_value(), mapped_lo.take_value()};
  }
  return Output(std::move(result));
}
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
struct Preparation final {
  ElementType source, target;
  ResourceVector<Bounds> bounds;
  std::optional<std::uint32_t> axis;
  bool rescale, clip, identity, materialize;
  bool fast_u8_f32 = false;
  bool fast_f32_u8 = false;
  bool fast_i64_u8 = false;
  bool fast_f64_f32 = false;
};
Status check_encoding(const std::optional<TensorEncoding>& encoding,
                      const Bounds& bounds) {
  if (!encoding)
    return Status::success();
  if (endpoint(encoding->stored[0]).compare(bounds.source_lo) != 0 ||
      endpoint(encoding->stored[1]).compare(bounds.source_hi) != 0)
    return invalid("declared source encoding conflicts with source range");
  return Status::success();
}
Result<std::optional<TensorEncoding>> effective_channel_encoding(
    const TensorDescription& description, std::size_t index) {
  std::optional<TensorEncoding> effective =
      description.channels[index].encoding;
  for (const auto& group : description.groups)
    for (std::size_t j = 0; j < group.indices.size(); ++j)
      if (group.indices[j] == index && group.components[j].encoding) {
        if (!effective)
          effective = group.components[j].encoding;
        else if (!(*effective == *group.components[j].encoding))
          return Result<std::optional<TensorEncoding>>(
              invalid("conflicting channel and group encodings"));
      }
  if (!effective)
    effective = description.encoding;
  return Result<std::optional<TensorEncoding>>(std::move(effective));
}
Result<TensorDescription> convert_description(
    TensorDescription description, const Preparation& state,
    const std::vector<std::uint64_t>& shape, bool respect) {
  using Output = Result<TensorDescription>;
  if (!state.rescale)
    return Output(std::move(description));
  if (state.axis) {
    if (!description.channel_axis)
      description.channel_axis = state.axis;
    if (description.channels.empty())
      description.channels.resize(shape[*state.axis]);
    for (std::size_t i = 0; i < description.channels.size(); ++i) {
      auto& channel = description.channels[i];
      const auto& bounds = state.bounds[i];
      auto resolved = effective_channel_encoding(description, i);
      if (!resolved.ok())
        return Output(resolved.status());
      const auto effective = resolved.take_value();
      if (respect) {
        auto check = check_encoding(effective, bounds);
        if (!check.ok())
          return Output(check);
      }
      auto mapping = mapped_encoding(effective, bounds, true);
      if (!mapping.ok())
        return Output(mapping.status());
      channel.encoding = mapping.take_value();
    }
    description.encoding.reset();
    for (auto& group : description.groups)
      for (std::size_t i = 0; i < group.indices.size(); ++i)
        group.components[i].encoding =
            description.channels[group.indices[i]].encoding;
  } else {
    const auto& bounds = state.bounds[0];
    if (description.channel_axis && description.channels.empty() &&
        !description.groups.empty())
      description.channels.resize(shape[*description.channel_axis]);
    if (description.component) {
      const auto effective = description.component->encoding
                                 ? description.component->encoding
                                 : description.encoding;
      if (respect) {
        auto check = check_encoding(effective, bounds);
        if (!check.ok())
          return Output(check);
      }
      auto mapping = mapped_encoding(effective, bounds, true);
      if (!mapping.ok())
        return Output(mapping.status());
      description.component->encoding = mapping.take_value();
    }
    if (description.channel_axis && !description.channels.empty()) {
      for (std::size_t i = 0; i < description.channels.size(); ++i) {
        auto& channel = description.channels[i];
        auto resolved = effective_channel_encoding(description, i);
        if (!resolved.ok())
          return Output(resolved.status());
        const auto effective = resolved.take_value();
        if (respect) {
          auto check = check_encoding(effective, bounds);
          if (!check.ok())
            return Output(check);
        }
        auto mapping = mapped_encoding(effective, bounds, true);
        if (!mapping.ok())
          return Output(mapping.status());
        channel.encoding = mapping.take_value();
      }
      for (auto& group : description.groups)
        for (std::size_t i = 0; i < group.indices.size(); ++i)
          group.components[i].encoding =
              description.channels[group.indices[i]].encoding;
    }
    if (respect) {
      auto check = check_encoding(description.encoding, bounds);
      if (!check.ok())
        return Output(check);
    }
    auto mapping = mapped_encoding(description.encoding, bounds, true);
    if (!mapping.ok())
      return Output(mapping.status());
    description.encoding = mapping.take_value();
  }
  return Output(std::move(description));
}
TensorDescription raw_description(TensorDescription description,
                                  const std::vector<std::uint64_t>& shape) {
  if (description.channel_axis && description.channels.empty() &&
      !description.groups.empty())
    description.channels.resize(shape[*description.channel_axis]);
  for (const auto& group : description.groups)
    for (std::size_t i = 0; i < group.indices.size(); ++i) {
      auto& channel = description.channels[group.indices[i]];
      const auto& component = group.components[i];
      if (channel.name.empty())
        channel.name = component.name;
      if (channel.role.empty())
        channel.role = component.role;
      if (channel.unit.empty())
        channel.unit = component.unit;
    }
  description.encoding.reset();
  if (description.component)
    description.component->encoding.reset();
  for (auto& channel : description.channels)
    channel.encoding.reset();
  description.groups.clear();
  return description;
}
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Parameters& params) {
  using Output = Result<OperationPreparation>;
  const auto& input = inputs[0];
  const auto& shape = input.descriptor.shape;
  if (!supported(input.descriptor.element_type) || shape.empty() ||
      shape.size() > 8)
    return Output(Status{ErrorCode::TypeMismatch,
                         "numeric.convert_format requires rank 1..8"});
  std::uint64_t elements = 1;
  for (const auto extent : shape) {
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
  state->materialize = layout == "materialize";
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
    if (s[0].compare(s[1]) >= 0 || t[0].compare(t[1]) == 0)
      return Output(
          invalid("source bounds must increase and target bounds must differ"));
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
  if (layout == "view" && input.planar_layout)
    return Output(
        invalid("ViewUnavailable: planar output requires publication"));

  const auto metadata_mode =
      params.count("metadata_mode")
          ? std::get<std::string>(params.at("metadata_mode"))
          : "respect";
  if (metadata_mode != "respect" && metadata_mode != "raw" &&
      metadata_mode != "override")
    return Output(invalid("invalid metadata_mode"));
  if ((metadata_mode == "override") !=
      static_cast<bool>(params.count("metadata_override")))
    return Output(
        invalid("metadata_override must occur exactly in override mode"));
  std::optional<TensorDescription> description;
  if (metadata_mode == "override") {
    auto decoded = tensor_description_from_parameter(
        std::get<std::string>(params.at("metadata_override")));
    if (!decoded.ok())
      return Output(decoded.status());
    description = decoded.take_value();
  } else {
    for (const auto& facet : input.facets)
      if (facet.key == "photospider.tensor-description") {
        auto decoded = decode_tensor_description(facet);
        if (!decoded.ok() && metadata_mode != "raw")
          return Output(decoded.status());
        if (decoded.ok())
          description = decoded.take_value();
      }
  }
  if (description) {
    auto valid = validate_tensor_description(*description, input.descriptor);
    if (!valid.ok() && metadata_mode != "raw")
      return Output(valid);
    if (!valid.ok())
      description.reset();
  }
  if (state->axis && metadata_mode != "raw" && description &&
      description->channel_axis && *state->axis != *description->channel_axis)
    return Output(invalid("range axis conflicts with tensor description"));
  OperationOutputSpecialization output;
  output.metadata = input;
  output.metadata.descriptor.element_type = state->target;
  output.metadata.facets.clear();
  // Facet propagation is filled by the metadata pass below. Opaque annotations
  // remain byte identical and retain their resource bindings.
  for (const auto& facet : input.facets)
    if (facet.key != "photospider.tensor-description")
      output.metadata.facets.push_back(facet);
  if (metadata_mode != "raw" && state->rescale && !description)
    description.emplace();
  if (description) {
    auto converted =
        metadata_mode == "raw"
            ? Result<TensorDescription>(raw_description(*description, shape))
            : convert_description(*description, *state, shape,
                                  metadata_mode == "respect");
    if (!converted.ok())
      return Output(converted.status());
    auto valid = validate_tensor_description(converted.value(),
                                             output.metadata.descriptor);
    if (!valid.ok())
      return Output(valid);
    auto encoded = encode_tensor_description(converted.value());
    if (!encoded.ok())
      return Output(encoded.status());
    output.metadata.facets.push_back(encoded.take_value());
  }
  if (input.planar_layout) {
    output.metadata.planar_layout = input.planar_layout;
    output.metadata.planar_layout->row_pitch_bytes = 0;
  }
  auto all = Footprint::all(shape);
  if (!all.ok())
    return Output(all.status());
  DependencyMappedNeed need;
  need.port = 0;
  need.roles = static_cast<std::uint32_t>(DependencyRole::Data);
  for (std::size_t i = 0; i < shape.size(); ++i) {
    DependencyAxis axis;
    axis.observation_axis = static_cast<std::int32_t>(i);
    need.axes.push_back(axis);
  }
  output.static_dependency_pieces =
      std::vector<DependencyMapPiece>{{all.take_value(), {std::move(need)}}};
  output.regional_atomic = true;
  output.preserve_output_views =
      state->identity && !state->materialize && !input.planar_layout;
  if (output.preserve_output_views)
    output.maximum_output_payload_bytes = 0;
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(state);
  return Output(std::move(result));
}

std::uint64_t raw(const std::uint8_t* source, ElementType type) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, source, sample_width(type));
  return bits;
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
std::string coordinate_text(const std::vector<std::uint64_t>& at) {
  std::string text = " coordinate=[";
  for (std::size_t i = 0; i < at.size(); ++i) {
    if (i)
      text += ',';
    text += std::to_string(at[i]);
  }
  return text + ']';
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
Status convert_one(const std::uint8_t* source, std::uint8_t* target,
                   const Preparation& state, const Bounds& b,
                   const std::vector<std::uint64_t>& at) {
  const auto input = raw(source, state.source);
  if (state.fast_u8_f32) {
    static const auto lookup = [] {
      std::array<std::uint32_t, 256> result{};
      for (unsigned i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint32_t>(
            Rational::divide(integer_value(i), integer_value(255))
                .floating_bits(true));
      return result;
    }();
    write_raw(target, state.target, lookup[input]);
    return Status::success();
  }
  if (state.fast_i64_u8) {
    write_raw(target, state.target, mapped_i64_u8(input));
    return Status::success();
  }
  if (state.fast_f32_u8) {
    const auto exponent = (input >> 23) & 255;
    if (exponent == 255)
      return sample_error(
          "nonfinite source for integer target" + coordinate_text(at),
          FailureReason::InvalidDomain);
    float sample;
    const auto bits = static_cast<std::uint32_t>(input);
    std::memcpy(&sample, &bits, 4);
    const double mapped = static_cast<double>(sample) * 255.0;
    if (mapped < -0.5 || mapped > 255.5) {
      if (!state.clip)
        return sample_error("integer overflow" + coordinate_text(at),
                            FailureReason::ArithmeticOverflow);
      write_raw(target, state.target, mapped < 0 ? 0 : 255);
      return Status::success();
    }
    const double lower = std::floor(mapped);
    const auto rounded =
        static_cast<std::int64_t>(lower) +
        (mapped - lower > .5 ||
         (mapped - lower == .5 && (static_cast<std::int64_t>(lower) & 1)));
    if ((rounded < 0 || rounded > 255) && !state.clip)
      return sample_error("integer overflow" + coordinate_text(at),
                          FailureReason::ArithmeticOverflow);
    write_raw(
        target, state.target,
        static_cast<std::uint64_t>(std::clamp<std::int64_t>(rounded, 0, 255)));
    return Status::success();
  }
  if (state.fast_f64_f32) {
    auto bits = narrow_f64_bits(input);
    const bool finite = ((input >> 52) & 2047) != 2047;
    if (state.rescale && (input & UINT64_C(0x7fffffffffffffff)) == 0) {
      const auto& bounds = state.bounds[0];
      if (bounds.source_lo.n.zero())
        bits = static_cast<std::uint32_t>(bounds.target_lo.floating_bits(true));
      else if (bounds.source_hi.n.zero())
        bits = static_cast<std::uint32_t>(bounds.target_hi.floating_bits(true));
      else
        bits = 0;
    }
    if (finite && (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
      if (!state.clip)
        return sample_error("floating overflow" + coordinate_text(at),
                            FailureReason::ArithmeticOverflow);
      bits = (bits & UINT32_C(0x80000000)) | UINT32_C(0x7f7fffff);
    }
    write_raw(target, state.target, bits);
    return Status::success();
  }
  if (state.source == state.target && b.identity) {
    write_raw(target, state.target, input);
    return Status::success();
  }
  Rational value;
  bool source_nonfinite = false;
  bool nan = false;
  bool sign = false;
  if (floating(state.source)) {
    const unsigned fraction = state.source == ElementType::Float32 ? 23 : 52;
    const std::uint64_t all = state.source == ElementType::Float32 ? 255 : 2047;
    const auto exponent = (input >> fraction) & all;
    sign = (input >> (fraction + (fraction == 23 ? 8 : 11))) != 0;
    source_nonfinite = exponent == all;
    nan = source_nonfinite &&
          (input & ((static_cast<std::uint64_t>(1) << fraction) - 1));
    if (!source_nonfinite)
      value = Rational::binary(input, fraction == 23);
  } else {
    switch (state.source) {
      case ElementType::UInt8:
        value = integer_value(input);
        break;
      case ElementType::UInt16:
        value = integer_value(input);
        break;
      case ElementType::Int8:
        value = Rational::integer(static_cast<std::int8_t>(input));
        break;
      case ElementType::Int16:
        value = Rational::integer(static_cast<std::int16_t>(input));
        break;
      case ElementType::Int64:
        value = Rational::integer(static_cast<std::int64_t>(input));
        break;
      default:
        break;
    }
  }
  if (source_nonfinite) {
    if (!floating(state.target))
      return sample_error(
          "nonfinite source for integer target" + coordinate_text(at),
          FailureReason::InvalidDomain);
    if (nan) {
      write_raw(target, state.target,
                nan_bits(input, state.source, state.target));
      return Status::success();
    }
    if (state.rescale && b.target_lo.compare(b.target_hi) > 0)
      sign = !sign;
    const bool small = state.target == ElementType::Float32;
    write_raw(target, state.target,
              (static_cast<std::uint64_t>(sign) << (small ? 31 : 63)) |
                  (static_cast<std::uint64_t>(small ? 255 : 2047)
                   << (small ? 23 : 52)));
    return Status::success();
  }
  if (state.rescale && !(b.identity && state.source == state.target)) {
    if (value.compare(b.source_lo) == 0) {
      value = b.target_lo;
      value.negative_zero = b.target_lo.negative_zero;
    } else if (value.compare(b.source_hi) == 0) {
      value = b.target_hi;
      value.negative_zero = b.target_hi.negative_zero;
    } else {
      const auto fraction =
          Rational::divide(Rational::subtract(value, b.source_lo),
                           Rational::subtract(b.source_hi, b.source_lo));
      value = Rational::add(
          b.target_lo,
          Rational::multiply(fraction,
                             Rational::subtract(b.target_hi, b.target_lo)));
      value.negative_zero = false;
    }
  }
  if (floating(state.target)) {
    const bool small = state.target == ElementType::Float32;
    auto bits = value.floating_bits(small);
    const auto exponent = (bits >> (small ? 23 : 52)) & (small ? 255 : 2047);
    if (exponent == (small ? 255 : 2047)) {
      if (!state.clip)
        return sample_error("floating overflow" + coordinate_text(at),
                            FailureReason::ArithmeticOverflow);
      bits = (bits & (static_cast<std::uint64_t>(1) << (small ? 31 : 63))) |
             (small ? UINT32_C(0x7f7fffff) : UINT64_C(0x7fefffffffffffff));
    }
    write_raw(target, state.target, bits);
    return Status::success();
  }
  const auto rounded = value.rounded_magnitude();
  const auto low = minimum(state.target);
  const auto high = maximum(state.target);
  Rational rounded_value{rounded, Natural(1), value.negative, false};
  if (rounded_value.compare(low) < 0 || rounded_value.compare(high) > 0) {
    if (!state.clip)
      return sample_error("integer overflow" + coordinate_text(at),
                          FailureReason::ArithmeticOverflow);
    rounded_value = rounded_value.compare(low) < 0 ? low : high;
  }
  const auto magnitude = rounded_value.n.low64();
  write_raw(target, state.target,
            rounded_value.negative ? UINT64_C(0) - magnitude : magnitude);
  return Status::success();
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

Result<ValueFragments> publish(const DependencyPhase& phase,
                               const Preparation& state) {
  using Output = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  numeric_ops::ArrayPublication publication(phase.query.outputs.boxes().size(),
                                            descriptor.shape.size());
  std::vector<Value> values;
  for (const auto& box : phase.query.outputs.boxes()) {
    if (state.identity && !state.materialize) {
      for (const auto& fragment : phase.inputs[0].fragments()) {
        std::vector<RegionDimension> overlap;
        bool intersects = true;
        for (std::size_t a = 0; a < box.rank(); ++a) {
          const auto x = fragment.region().dimensions()[a];
          const auto y = box.dimensions()[a];
          const auto begin = std::max(x.offset, y.offset);
          const auto end = std::min(x.offset + x.extent, y.offset + y.extent);
          if (begin >= end) {
            intersects = false;
            break;
          }
          overlap.push_back({begin, end - begin});
        }
        if (!intersects)
          continue;
        auto view = fragment.view(Region(std::move(overlap)));
        if (!view.ok())
          return Output(view.status());
        auto mapped = Value::from_storage(
            descriptor, view.value().region(), view.value().layout(),
            view.value().storage(), facets, phase.query.resources);
        if (!mapped.ok())
          return Output(mapped.status());
        auto retained = publication.retain(mapped.take_value());
        if (!retained.ok())
          return Output(retained.status());
        values.push_back(retained.take_value());
      }
      continue;
    }
    auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
    if (!allocated.ok())
      return Output(allocated.status());
    auto writer = allocated.take_value();
    const auto target_width = Value::element_size(state.target);
    for (const auto& fragment : phase.inputs[0].fragments()) {
      std::vector<RegionDimension> overlap;
      bool intersects = true;
      for (std::size_t a = 0; a < box.rank(); ++a) {
        const auto x = fragment.region().dimensions()[a];
        const auto y = box.dimensions()[a];
        const auto begin = std::max(x.offset, y.offset);
        const auto end = std::min(x.offset + x.extent, y.offset + y.extent);
        if (begin >= end) {
          intersects = false;
          break;
        }
        overlap.push_back({begin, end - begin});
      }
      if (!intersects)
        continue;
      auto footprint = Footprint::from_regions(
          descriptor.shape, {Region(std::move(overlap))}, phase.sets);
      if (!footprint.ok())
        return Output(footprint.status());
      auto status = footprint.value().visit(
          [&](const auto& at) {
            auto charged = phase.consume_work(at.size() + 64);
            if (!charged.ok())
              return charged;
            std::uint64_t linear = 0;
            for (std::size_t a = 0; a < at.size(); ++a)
              linear = linear * box.dimensions()[a].extent + at[a] -
                       box.dimensions()[a].offset;
            auto address = fragment.byte_address(at);
            if (!address.ok())
              return address.status();
            const auto& bounds = state.bounds[state.axis ? at[*state.axis] : 0];
            return convert_one(fragment.bytes().data() + address.value(),
                               writer.data() + linear * target_width, state,
                               bounds, at);
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Output(status);
    }
    auto value = std::move(writer).publish(facets, phase.query.resources);
    if (!value.ok())
      return Output(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Output(retained.status());
    values.push_back(retained.take_value());
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}

struct Continuation final {
  const Preparation* state;
  bool requested = false;
  explicit Continuation(const Preparation* prepared) : state(prepared) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    FloatingEnvironment environment;
    ExactWorkScope exact_work(&phase.consume_work, &phase.query.cancellation);
    try {
      auto result = publish(phase, *state);
      return result.ok() ? Result<DependencyPoll>(result.take_value())
                         : Result<DependencyPoll>(result.status());
    } catch (const ExactWorkFailure& failure) {
      return Result<DependencyPoll>(failure.status);
    }
  }
};

Status planar(const PlanarOperationInvocation& call) {
  FloatingEnvironment environment;
  OperationMetadata input;
  input.descriptor = call.inputs[0].descriptor();
  input.facets = call.inputs[0].facets();
  const auto& config = call.inputs[0].config();
  input.planar_layout = PlanarImageLayout{
      config.order,        config.height_axis,     config.width_axis,
      config.channel_axis, config.row_pitch_bytes, config.groups};
  auto prepared = prepare({input}, call.parameters);
  if (!prepared.ok())
    return prepared.status();
  const auto& state =
      *static_cast<const Preparation*>(prepared.value().state.get());
  const auto& output_layout =
      *prepared.value().outputs[0].metadata.planar_layout;
  const auto& dimensions = call.output_region.dimensions();
#if defined(__x86_64__)
  const bool use_avx2 = __builtin_cpu_supports("avx2");
#endif
  const auto height = output_layout.height_axis;
  const auto width = output_layout.width_axis;
  const auto channels = output_layout.channel_axis;
  std::vector<std::uint64_t> at(dimensions.size());
  for (std::size_t a = 0; a < at.size(); ++a)
    at[a] = dimensions[a].offset;
  const auto channel_start = channels ? dimensions[*channels].offset : 0;
  const auto channel_end =
      channels ? channel_start + dimensions[*channels].extent : 1;
  for (std::uint64_t channel = channel_start; channel < channel_end;
       ++channel) {
    if (channels)
      at[*channels] = channel;
    for (std::uint64_t row = dimensions[height].offset;
         row < dimensions[height].offset + dimensions[height].extent;) {
      at[height] = row;
      std::uint64_t advanced_rows =
          dimensions[height].offset + dimensions[height].extent - row;
      for (std::uint64_t column = dimensions[width].offset;
           column < dimensions[width].offset + dimensions[width].extent;) {
        if (call.cancellation.cancelled())
          return Status{ErrorCode::Cancelled, "numeric conversion cancelled"};
        at[height] = row;
        at[width] = column;
        auto read = call.inputs[0].rectangle_run(at);
        if (!read.ok())
          return read.status();
        auto write = call.output.rectangle_run(at);
        if (!write.ok())
          return write.status();
        const auto& src = read.value();
        const auto& dst = write.value();
        const auto rows = std::min({src.rows, dst.rows, advanced_rows});
        advanced_rows = std::min(advanced_rows, rows);
        const auto samples = std::min(src.row.samples, dst.row.samples);
#if defined(PHOTOSPIDER_HAS_CONVERSION_SME)
        // A bounded, fully requested contiguous rectangle can be admitted and
        // converted in one streaming scope. Partial-width/padded windows retain
        // the row path. Failed admission writes nothing and uses exact
        // fallback.
        const auto tile_samples = rows * samples;
        if (state.fast_f32_u8 && tile_samples >= 4096 &&
            tile_samples <= 65536 && src.row_stride_bytes == samples * 4 &&
            dst.row_stride_bytes == samples && sme_conversion_available()) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, "numeric conversion cancelled"};
          if (const auto* budget = resource_internal::metadata_budget()) {
            auto charged = budget->consume({tile_samples});
            if (!charged.ok())
              return charged;
          }
          const auto polling =
              execution_internal::CancellationPoll::borrow(call.cancellation);
          const auto used = format_numeric::sme_f32_u8_tile(
              src.row.data, dst.row.data, tile_samples, polling);
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, "numeric conversion cancelled"};
          if (used == tile_samples) {
            column += samples;
            continue;
          }
        }
#endif
        for (std::uint64_t dy = 0; dy < rows; ++dy) {
          at[height] = row + dy;
          const auto* source = src.row.data + dy * src.row_stride_bytes;
          auto* target = dst.row.data + dy * dst.row_stride_bytes;
          const auto source_width = sample_width(state.source);
          const auto target_width = sample_width(state.target);
          for (std::uint64_t dx = 0; dx < samples;) {
            if ((dx & 63) == 0 && call.cancellation.cancelled())
              return Status{ErrorCode::Cancelled,
                            "numeric conversion cancelled"};
            if ((dx & 63) == 0) {
              if (const auto* budget = resource_internal::metadata_budget()) {
                auto charged = budget->consume({64});
                if (!charged.ok())
                  return charged;
              }
            }
            const auto count =
                std::min<std::uint64_t>(samples - dx, 64 - (dx & 63));
            if (state.fast_i64_u8) {
              i64_u8_span(source + dx * source_width,
                          target + dx * target_width, count);
              dx += count;
              continue;
            }
#if defined(__aarch64__) || defined(__x86_64__)
            std::uint64_t vectorized = 0;
#if defined(__aarch64__)
            if (state.fast_u8_f32)
              vectorized = neon_u8_f32(source + dx * source_width,
                                       target + dx * target_width, count);
            else if (state.fast_f32_u8)
              vectorized = neon_f32_u8(source + dx * source_width,
                                       target + dx * target_width, count);
            else if (state.fast_f64_f32)
              vectorized = neon_f64_f32(source + dx * source_width,
                                        target + dx * target_width, count);
#else
            if (use_avx2) {
              if (state.fast_u8_f32)
                vectorized = avx2_u8_f32(source + dx * source_width,
                                         target + dx * target_width, count);
              else if (state.fast_f32_u8)
                vectorized = avx2_f32_u8(source + dx * source_width,
                                         target + dx * target_width, count);
              else if (state.fast_f64_f32)
                vectorized = avx2_f64_f32(source + dx * source_width,
                                          target + dx * target_width, count);
            }
#endif
            dx += vectorized;
            if (vectorized == count)
              continue;
#endif
            at[width] = column + dx;
            const auto& bounds = state.bounds[state.axis ? at[*state.axis] : 0];
            auto status =
                convert_one(source + dx * source_width,
                            target + dx * target_width, state, bounds, at);
            if (!status.ok())
              return status;
            ++dx;
          }
        }
        column += samples;
      }
      row += advanced_rows;
    }
  }
  return Status::success();
}

OperationDefinition conversion() {
  OperationDefinition definition;
  definition.key = "numeric.convert_format_strict";
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.planar_storage_capable = true;
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"dtype", OperationParameterType::String},
      {"rescale", OperationParameterType::Bool, false},
      {"source_range", OperationParameterType::String, false},
      {"target_range", OperationParameterType::String, false},
      {"axis", OperationParameterType::Int64, false},
      {"rounding", OperationParameterType::String, false},
      {"overflow", OperationParameterType::String, false},
      {"metadata_mode", OperationParameterType::String, false},
      {"metadata_override", OperationParameterType::String, false},
      {"layout", OperationParameterType::String, false}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Continuation);
  output.maximum_dependency_stages = 2;
  definition.prepare_static = [](const auto& inputs, const auto& params) {
    return prepare(inputs, params);
  };
  definition.start_dependency = [](const DependencyQuery& query,
                                   const BufferAllocator& allocator) {
    const auto* state =
        static_cast<const Preparation*>(query.prepared->state());
    return DependencyContinuation::make<Continuation>(allocator, state);
  };
  definition.planar_callback = [](const PlanarOperationInvocation& call) {
    return planar(call);
  };
  return definition;
}
}  // namespace

Status register_numeric_conversion(OperationRegistry* registry) {
  return registry->register_operation(conversion());
}
}  // namespace ps::plugin_internal
