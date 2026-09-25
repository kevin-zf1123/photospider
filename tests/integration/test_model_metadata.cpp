#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/tensor_description.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
TensorDescription gray() {
  TensorDescription d;
  d.channel_axis = 0;
  d.channels = {{"Y", "gray", "relative"}};
  d.channels[0].interpretation.emplace();
  auto& channel = *d.channels[0].interpretation;
  channel.model = "gray";
  channel.coordinates = TensorModelCoordinates{"relative", "", "", {}};
  TensorColorGroup group;
  group.name = "gray";
  group.indices = {0};
  group.components = {{"Y", "gray", "relative"}};
  group.interpretation.model = "gray";
  group.interpretation.coordinates =
      TensorModelCoordinates{"", "1931-2", "linear_y", {}};
  d.groups = {group};
  return d;
}
void overlapping_assertions() {
  auto d = gray();
  const auto first = take(encode_tensor_description(d));
  require(first.version == 5, "complementary coordinates require v5");
  const auto roundtrip = take(decode_tensor_description(first));
  require(take(encode_tensor_description(roundtrip)).payload == first.payload,
          "v5 canonical roundtrip");
  require(roundtrip.channels[0].interpretation->coordinates->gray_kind.empty(),
          "codec must not silently rewrite incomplete provenance");
  require(!(*d.channels[0].interpretation->coordinates ==
            *d.groups[0].interpretation.coordinates),
          "exact equality must remain different from compatibility");
  std::swap(d.channels[0].interpretation->coordinates,
            d.groups[0].interpretation.coordinates);
  require(encode_tensor_description(d).ok(), "complementarity is symmetric");
  d = gray();
  d.groups[0].interpretation.coordinates->scale = "absolute";
  require(!encode_tensor_description(d).ok(), "contradictory scale rejected");
  d = gray();
  d.channels[0].interpretation->coordinates->observer = "1964-10";
  require(!encode_tensor_description(d).ok(),
          "contradictory observer rejected");
  d = gray();
  d.channels[0].interpretation->coordinates->gray_kind = "oklab_l";
  require(!encode_tensor_description(d).ok(),
          "contradictory gray kind rejected");
  d = gray();
  d.channels[0].interpretation->coordinates->ncl_coefficients =
      std::array<double, 2>{.25, .25};
  require(encode_tensor_description(d).ok(), "one-sided NCL is compatible");
  d.groups[0].interpretation.coordinates->ncl_coefficients =
      std::array<double, 2>{.3, .2};
  require(!encode_tensor_description(d).ok(), "contradictory NCL rejected");

  // Every ordering must retain a contradiction across a declaration which
  // supplies neither side of the conflicting field.
  d = gray();
  d.channels[0].interpretation.reset();
  d.groups.resize(3, d.groups[0]);
  for (unsigned i = 0; i < 3; ++i) {
    d.groups[i].name = "gray" + std::to_string(i);
    d.groups[i].interpretation.coordinates.emplace();
  }
  d.groups[0].interpretation.coordinates->scale = "relative";
  d.groups[1].interpretation.coordinates.reset();
  d.groups[2].interpretation.coordinates->scale = "absolute";
  std::array<unsigned, 3> permutation{0, 1, 2};
  const auto groups = d.groups;
  do {
    for (unsigned i = 0; i < 3; ++i)
      d.groups[i] = groups[permutation[i]];
    require(!encode_tensor_description(d).ok(),
            "overlapping assertion lost across intermediate empty group");
  } while (std::next_permutation(permutation.begin(), permutation.end()));

  // The same accumulation rule applies to older interpretation fields too.
  d = gray();
  d.groups[0].interpretation.coordinates.reset();
  d.channels[0].interpretation->coordinates.reset();
  d.channels[0].interpretation->primaries = "srgb";
  auto last = d.groups[0];
  last.name = "other";
  last.interpretation.primaries = "display-p3";
  d.groups.push_back(last);
  require(!encode_tensor_description(d).ok(),
          "earlier primaries assertion must survive empty group");
}
void canonical_and_malformed() {
  TensorDescription d;
  d.coordinates = TensorModelCoordinates{"relative", "1931-2", "encoded_luma",
                                         std::array<double, 2>{.2126, .0722}};
  const auto valid = take(encode_tensor_description(d));
  const auto parameter = take(tensor_description_parameter(d));
  require(take(encode_tensor_description(
                   take(tensor_description_from_parameter(parameter))))
                  .payload == valid.payload,
          "parameter preserves v5 bytes");
  std::size_t rejected = 0;
  for (std::size_t length = 0; length < valid.payload.size(); ++length) {
    auto truncated = valid;
    truncated.payload.resize(length);
    require(!decode_tensor_description(truncated).ok(), "truncation accepted");
    ++rejected;
  }
  const auto reject = [&](ValueFacet invalid) {
    require(!decode_tensor_description(invalid).ok(), "malformed v5 accepted");
    ++rejected;
  };
  auto invalid = valid;
  invalid.payload.push_back(0);
  reject(invalid);
  invalid = valid;
  invalid.version = 4;
  reject(invalid);
  invalid = valid;
  invalid.payload[3] = '4';
  reject(invalid);
  const auto& c = *d.coordinates;
  const auto record_size = 1 + 1 + c.scale.size() + 1 + c.observer.size() + 1 +
                           c.gray_kind.size() + 1 + 16;
  const auto start = valid.payload.size() - record_size;
  invalid = valid;
  invalid.payload[start] = 2;
  reject(invalid);
  invalid = valid;
  invalid.payload[valid.payload.size() - 17] = 2;
  reject(invalid);
  invalid = valid;
  invalid.payload[start + 3 + c.scale.size()] = 0xff;
  reject(invalid);
  invalid = valid;
  const std::uint64_t nan = UINT64_C(0x7ff8000000000042);
  for (unsigned i = 0; i < 8; ++i)
    invalid.payload[valid.payload.size() - 16 + i] =
        static_cast<std::uint8_t>(nan >> (8 * i));
  reject(invalid);
  d.coordinates->scale = "unknown";
  require(!encode_tensor_description(d).ok(), "unknown scale accepted");
  d.coordinates->scale = "relative";
  d.coordinates->gray_kind = "unknown";
  require(!encode_tensor_description(d).ok(), "unknown gray kind accepted");
  d.coordinates->gray_kind = "linear_y";
  d.coordinates->observer.assign(129, 'x');
  require(!encode_tensor_description(d).ok(), "oversized observer accepted");
  d.coordinates.emplace();
  const auto empty = take(encode_tensor_description(d));
  require(
      empty.version == 5 && take(decode_tensor_description(empty)).coordinates,
      "present empty coordinates preserve v5 identity");
  d.coordinates.reset();
  const auto legacy = take(encode_tensor_description(d));
  require(legacy.version == 4 &&
              !take(decode_tensor_description(legacy)).coordinates,
          "absent coordinates retain v4");
  require(
      take(encode_tensor_description(take(decode_tensor_description(legacy))))
              .payload == legacy.payload,
      "v4 bytes unchanged");
  std::cout << rejected << " malformed/truncated TDM5 payloads rejected\n";
}
}  // namespace

int main() try {
  overlapping_assertions();
  canonical_and_malformed();
  std::cout
      << "coordinate complement/conflict/permutation and TDM4/TDM5 passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
