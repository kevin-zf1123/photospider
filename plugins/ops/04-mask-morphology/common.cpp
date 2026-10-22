#include "04-mask-morphology/common.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal::mask {
namespace {
std::int64_t signed_integer(const std::string& text) {
  std::int64_t n = 0;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
  parameter(!text.empty() && r.ec == std::errc{} &&
                r.ptr == text.data() + text.size() &&
                std::to_string(n) == text && n != INT64_MIN,
            "noncanonical footprint coordinate");
  return n;
}
}  // namespace
Footprint::Footprint(const Parameters& p) : kind(text(p, "footprint")) {
  parameter(kind == "square" || kind == "diamond" || kind == "disk" ||
                kind == "custom",
            "unknown mask footprint");
  if (kind != "custom") {
    parameter(p.count("radius") && !p.count("offsets"),
              "analytic footprint requires radius only");
    ry = rx = integer(p, "radius");
    // Radius determines static support and scratch dimensions.
    parameter(ry >= 0 && ry <= (INT64_MAX - (INT64_C(1) << 40)) / 8,
              "footprint support arithmetic overflow");
    return;
  }
  parameter(p.count("offsets") && !p.count("radius"),
            "custom footprint requires offsets only");
  const auto encoded = text(p, "offsets");
  parameter(!encoded.empty(), "empty footprint");
  std::size_t pos = 0;
  while (pos < encoded.size()) {
    const auto end = encoded.find(';', pos);
    const auto item = encoded.substr(
        pos, end == std::string::npos ? std::string::npos : end - pos);
    const auto colon = item.find(':');
    parameter(colon != std::string::npos &&
                  item.find(':', colon + 1) == std::string::npos,
              "footprint pair syntax");
    const auto y = signed_integer(item.substr(0, colon));
    const auto x = signed_integer(item.substr(colon + 1));
    const auto pair = std::make_pair(y, x);
    parameter(custom.empty() || custom.back() < pair,
              "footprint offsets must be sorted and unique");
    custom.push_back(pair);
    ry = std::max(ry, std::abs(y));
    rx = std::max(rx, std::abs(x));
    if (end == std::string::npos)
      break;
    pos = end + 1;
    parameter(pos < encoded.size(), "trailing footprint separator");
  }
  parameter(std::binary_search(custom.begin(), custom.end(),
                               std::make_pair(INT64_C(0), INT64_C(0))),
            "footprint requires center");
  for (auto b : custom)
    parameter(std::binary_search(custom.begin(), custom.end(),
                                 std::make_pair(-b.first, -b.second)),
              "footprint must be symmetric");
  parameter(std::max(rx, ry) <= (INT64_MAX - (INT64_C(1) << 40)) / 8,
            "footprint support arithmetic overflow");
}
std::int64_t Footprint::row_radius(std::int64_t y) const {
  if (kind == "square")
    return rx;
  if (kind == "diamond")
    return rx - std::abs(y);
  const auto remaining =
      static_cast<unsigned __int128>(rx) * rx -
      static_cast<unsigned __int128>(std::abs(y)) * std::abs(y);
  std::uint64_t low = 0, high = static_cast<std::uint64_t>(rx);
  while (low < high) {
    const auto mid = low + (high - low + 1) / 2;
    if (static_cast<unsigned __int128>(mid) * mid <= remaining)
      low = mid;
    else
      high = mid - 1;
  }
  return static_cast<std::int64_t>(low);
}
Region Footprint::support(const Region& q, std::uint64_t h, std::uint64_t w,
                          unsigned stages) const {
  const auto s = spatial_region(q).dimensions();
  const auto axis = [stages](RegionDimension d, std::uint64_t n,
                             std::int64_t r) {
    const auto halo = static_cast<std::uint64_t>(r) * stages;
    const auto begin = d.offset > halo ? d.offset - halo : 0;
    const auto end = std::min(n, d.offset + d.extent + halo);
    return RegionDimension{begin, end - begin};
  };
  return Region({axis(s[0], h, ry), axis(s[1], w, rx)});
}
OperationDefinition definition(const char* name, unsigned ports,
                               std::vector<Parameter> parameters,
                               generation::Prepare prepare, bool valid_output) {
  std::vector<std::pair<std::string, SchemaTemplate>> outputs{
      {"values", spatial_schema(ElementType::Float64, {1, 1})}};
  if (valid_output)
    outputs.emplace_back("valid", spatial_schema(ElementType::Float64, {1, 1}));
  return generation::operation(std::string("mask.") + name + "_strict", ports,
                               std::move(parameters), outputs,
                               std::move(prepare));
}
void validate_description(const OperationMetadata& input, const Parameters& p,
                          const std::vector<std::uint64_t>& selected,
                          bool required, bool lab, bool hue) {
  if (!p.count("input_description")) {
    parameter(!required, "mask requires input_description");
    return;
  }
  const auto d =
      take(tensor_description_from_parameter(text(p, "input_description")));
  check(validate_tensor_description(d, tensor(input).descriptor));
  const auto axis = tensor(input).descriptor.shape.size() == 2 ? 1U : 0U;
  require(d.channel_axis == std::optional<std::uint32_t>{axis},
          "mask description channel axis mismatch");
  const TensorColorGroup* found = nullptr;
  for (const auto& group : d.groups) {
    bool match = hue ? std::all_of(selected.begin(), selected.end(),
                                   [&](auto c) {
                                     return std::find(group.indices.begin(),
                                                      group.indices.end(),
                                                      c) != group.indices.end();
                                   })
                     : group.indices == selected;
    if (match) {
      require(!found, "ambiguous mask color group");
      found = &group;
    }
  }
  require(found, "selector must match a complete color group");
  Parameters semantic{{"input_description", text(p, "input_description")},
                      {"group", found->name}};
  color_point::Selection s;
  try {
    s = color_point::select(input, semantic, {}, false);
    if (s.interpretation.model != "cmyk" &&
        (s.interpretation.profile || s.interpretation.configured))
      s = color_point::select(input, semantic);
  } catch (const basis_ops::CheckedFailure& failure) {
    throw failure.status;
  }
  const auto& model = s.interpretation.model;
  require(!model.empty(), "mask color model required");
  if (model == "cmyk")
    require(s.interpretation.convention == "icc-native" &&
                s.interpretation.reference == "profile" &&
                s.interpretation.profile,
            "mask CMYK requires native profile coordinates");
  else
    require(s.interpretation.convention == "relative-v1",
            "mask color requires native relative coordinates");
  for (unsigned j = 0; j < s.units.size(); ++j) {
    const auto& role = found->components[j].role;
    const auto& unit = s.units[j];
    if (role == "h" || role == "hue")
      require(unit == "turn" || unit == "turns" || unit == "radian" ||
                  unit == "pi_multiple",
              "mask hue requires explicit native units");
    else
      require(unit.empty() || unit == "1" || unit == "relative" ||
                  ((model == "cielab" || model == "cielch") &&
                   ((role == "a" && unit == "a*") ||
                    (role == "b" && unit == "b*") ||
                    (role == "c" && unit == "C*"))),
              "mask coordinates require native units");
  }
  if (lab)
    require(model == "cielab" && s.channels.size() == 3 &&
                found->components[0].role == "l" &&
                found->components[1].role == "a" &&
                found->components[2].role == "b",
            "mask requires normalized CIELAB l,a,b");
  if (hue) {
    require(model == "hsl" || model == "hsv" || model == "cielch" ||
                model == "oklch",
            "mask hue requires a polar model");
    const auto h =
        std::find(s.channels.begin(), s.channels.end(), selected[0]) -
        s.channels.begin();
    const auto c =
        std::find(s.channels.begin(), s.channels.end(), selected[1]) -
        s.channels.begin();
    require((found->components[h].role == "h" ||
             found->components[h].role == "hue") &&
                (s.units[h] == "turn" || s.units[h] == "turns") &&
                (found->components[c].role == "c" ||
                 found->components[c].role == "saturation"),
            "mask hue selector requires hue turns then chroma/saturation");
  } else if (required) {
    for (const auto& c : found->components)
      require(c.role != "h" && c.role != "hue",
              "periodic color coordinates cannot be fitted");
    require(s.channels == selected, "mask color coordinate order mismatch");
  }
}
}  // namespace ps::plugin_internal::mask
