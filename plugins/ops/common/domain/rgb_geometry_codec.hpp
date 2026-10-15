#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "common/domain/rgb_geometry.hpp"
#include "photospider/ops/format/rgb_geometry.hpp"
namespace ps::plugin_internal::basis_ops {
struct CheckedFailure final {
  Status status;
};
inline Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          "FMT-10: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status mismatch(const std::string& message) {
  return {ErrorCode::TypeMismatch,
          "FMT-10: " + message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline void require(bool condition, const std::string& message) {
  if (!condition)
    throw CheckedFailure{invalid(message)};
}
inline void typed(bool condition, const std::string& message) {
  if (!condition)
    throw CheckedFailure{mismatch(message)};
}
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw CheckedFailure{result.status()};
  return result.take_value();
}
inline void checked(Status result) {
  if (!result.ok())
    throw CheckedFailure{std::move(result)};
}
struct ResolvedBasis final {
  std::string preset;
  std::array<double, 6> xy;
  std::array<double, 2> white;
};
inline ResolvedBasis preset(const std::string& key) {
  if (key == "srgb_rec709")
    return {key, {.64, .33, .30, .60, .15, .06}, {.3127, .3290}};
  if (key == "display_p3")
    return {key, {.68, .32, .265, .69, .15, .06}, {.3127, .3290}};
  if (key == "rec2020")
    return {key, {.708, .292, .170, .797, .131, .046}, {.3127, .3290}};
  if (key == "adobe_rgb_1998")
    return {key, {.64, .33, .21, .71, .15, .06}, {.3127, .3290}};
  if (key == "prophoto_rgb")
    return {key,
            {.734699, .265301, .159597, .840403, .036598, .000105},
            {.3457, .3585}};
  if (key == "aces_ap0")
    return {key, {.73470, .26530, 0, 1, .00010, -.077}, {.32168, .33767}};
  if (key == "aces_ap1")
    return {key, {.713, .293, .165, .830, .128, .044}, {.32168, .33767}};
  throw CheckedFailure{invalid("unknown basis preset")};
}
inline ResolvedBasis resolve(const format::RgbBasis& basis) {
  ResolvedBasis out;
  if (!basis.preset.empty()) {
    out = preset(basis.preset);
    require(!basis.primaries_xy || *basis.primaries_xy == out.xy,
            "preset/custom primaries disagree");
    require(!basis.white || *basis.white == out.white,
            "preset/custom white disagree");
  } else {
    require(basis.primaries_xy && basis.white,
            "custom basis requires all xy coordinates");
    out = {"", *basis.primaries_xy, *basis.white};
  }
  for (auto& value : out.xy) {
    require(finite_bits(bits(value), false), "nonfinite primary xy");
    if (value == 0)
      value = 0;  // Metadata zero canonicalization.
  }
  for (auto& value : out.white) {
    require(finite_bits(bits(value), false), "nonfinite white xy");
    if (value == 0)
      value = 0;
  }
  return out;
}
inline std::string hex_word(double x) {
  auto raw = bits(x);
  if (!(raw & UINT64_C(0x7fffffffffffffff)))
    raw = 0;
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (unsigned i = 0; i < 16; ++i)
    out[i] = digits[(raw >> ((15 - i) * 4)) & 15];
  return out;
}
inline std::vector<double> parse_words(const std::string& value,
                                       const std::string& prefix,
                                       unsigned count) {
  require(value.size() == prefix.size() + count * 17 - 1 &&
              value.compare(0, prefix.size(), prefix) == 0,
          "malformed geometry codec");
  std::vector<double> out;
  for (unsigned j = 0; j < count; ++j) {
    if (j)
      require(value[prefix.size() + j * 17 - 1] == ',',
              "malformed xy separator");
    std::uint64_t raw = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const char c = value[prefix.size() + j * 17 + i];
      require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
              "noncanonical xy hex");
      raw =
          (raw << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    require(finite_bits(raw, false) && raw != UINT64_C(0x8000000000000000),
            "xy must be finite and zero-canonical");
    double x;
    std::memcpy(&x, &raw, 8);
    out.push_back(x);
  }
  return out;
}
inline std::string basis_codec(const ResolvedBasis& basis) {
  if (!basis.preset.empty())
    return basis.preset;
  std::string out = "basis-v1:";
  for (unsigned j = 0; j < 8; ++j) {
    if (j)
      out += ',';
    out += hex_word(j < 6 ? basis.xy[j] : basis.white[j - 6]);
  }
  return out;
}
inline ResolvedBasis parse_basis(const std::string& value) {
  if (value.compare(0, 9, "basis-v1:"))
    return preset(value);
  const auto words = parse_words(value, "basis-v1:", 8);
  ResolvedBasis out;
  std::copy_n(words.begin(), 6, out.xy.begin());
  std::copy_n(words.begin() + 6, 2, out.white.begin());
  return out;
}
inline std::array<double, 2> parse_white(const std::string& value) {
  if (value == "d65")
    return {.3127, .3290};
  if (value == "d50")
    return {.3457, .3585};
  if (value == "aces")
    return {.32168, .33767};
  const auto words = parse_words(value, "xy-v1:", 2);
  return {words[0], words[1]};
}
inline std::string white_codec(const std::array<double, 2>& white) {
  return "xy-v1:" + hex_word(white[0]) + ',' + hex_word(white[1]);
}
}  // namespace ps::plugin_internal::basis_ops
