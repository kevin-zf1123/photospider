#include "photospider/data/tensor_description.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "plugin/utf8_validation.hpp"

namespace ps {
namespace {
constexpr char kKey[] = "photospider.tensor-description";
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
bool valid_text(const std::string& text) {
  return text.size() <= 128 &&
         (text.empty() || plugin_internal::valid_utf8_key(text));
}
bool valid_interpretation(const TensorInterpretation& v) {
  for (const auto* text :
       {&v.model, &v.primaries, &v.transfer, &v.reference, &v.association})
    if (!valid_text(*text))
      return false;
  if (v.white)
    for (auto number : *v.white)
      if (!std::isfinite(number))
        return false;
  if (v.primaries_xy)
    for (auto number : *v.primaries_xy)
      if (!std::isfinite(number))
        return false;
  return true;
}
bool valid_channel(const TensorChannelDescription& channel) {
  return valid_text(channel.name) && valid_text(channel.role) &&
         valid_text(channel.unit) &&
         (!channel.interpretation ||
          valid_interpretation(*channel.interpretation));
}
Status validate_structure(const TensorDescription& value) {
  if ((value.channel_axis && *value.channel_axis >= 8) ||
      value.channels.size() > 65535 || value.axes.size() > 8 ||
      (!value.channel_axis && !value.channels.empty()) ||
      (value.component && !valid_channel(*value.component)))
    return invalid("invalid tensor description structure");
  for (const auto& channel : value.channels)
    if (!valid_channel(channel))
      return invalid("invalid tensor channel text");
  for (const auto& axis : value.axes)
    if (!valid_text(axis.name) || !valid_text(axis.unit) ||
        !std::isfinite(axis.origin) || !std::isfinite(axis.step) ||
        axis.step <= 0)
      return invalid("invalid tensor axis description");
  for (const auto* field : {&value.model, &value.primaries, &value.transfer,
                            &value.reference, &value.association})
    if (!valid_text(*field))
      return invalid("invalid tensor interpretation text");
  if (value.groups.size() > 128)
    return invalid("too many color groups");
  for (const auto& group : value.groups) {
    if (!valid_text(group.name) || group.name.empty() ||
        group.indices.empty() ||
        group.indices.size() != group.components.size() ||
        group.indices.size() > 64 ||
        !valid_interpretation(group.interpretation) ||
        group.interpretation.model.empty() ||
        (!group.interpretation.association.empty() &&
         group.interpretation.association != "straight"))
      return invalid("invalid explicit color group");
    const std::map<std::string, std::vector<std::string>> roles{
        {"rgb", {"red", "green", "blue"}},
        {"xyz", {"x", "y", "z"}},
        {"cielab", {"l", "a", "b"}},
        {"cielch", {"l", "c", "h"}},
        {"oklab", {"l", "a", "b"}},
        {"oklch", {"l", "c", "h"}},
        {"hsl", {"hue", "saturation", "lightness"}},
        {"hsv", {"hue", "saturation", "value"}},
        {"ycbcr", {"y", "cb", "cr"}},
        {"xyy", {"x", "y", "luminance"}},
        {"cmyk", {"cyan", "magenta", "yellow", "black"}},
        {"gray", {"gray"}},
        {"black_white", {"gray"}}};
    const auto expected = roles.find(group.interpretation.model);
    if (expected == roles.end() ||
        expected->second.size() != group.components.size())
      return invalid("unknown or incomplete color-group model");
    std::set<std::string> actual_roles;
    for (const auto& c : group.components)
      actual_roles.insert(c.role);
    if (actual_roles !=
        std::set<std::string>(expected->second.begin(), expected->second.end()))
      return invalid("incomplete color-group roles");
    const auto& interpretation = group.interpretation;
    if ((interpretation.model == "rgb" &&
         (interpretation.transfer.empty() ||
          (interpretation.primaries.empty() &&
           (!interpretation.primaries_xy || !interpretation.white)))) ||
        ((interpretation.model == "cielab" ||
          interpretation.model == "cielch" || interpretation.model == "xyz") &&
         !interpretation.white) ||
        (interpretation.model == "cmyk" && !interpretation.profile))
      return invalid("incomplete color-group interpretation");
    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < group.indices.size(); ++i)
      if (!seen.insert(group.indices[i]).second ||
          !valid_channel(group.components[i]) ||
          group.components[i].interpretation ||
          (group.alpha && *group.alpha == group.indices[i]))
        return invalid("invalid group component indices");
  }
  const auto compatible_text = [](const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
  };
  const auto compatible_interpretation = [&](const TensorInterpretation& a,
                                             const TensorInterpretation& b) {
    return compatible_text(a.model, b.model) &&
           compatible_text(a.primaries, b.primaries) &&
           compatible_text(a.transfer, b.transfer) &&
           compatible_text(a.reference, b.reference) &&
           compatible_text(a.association, b.association) &&
           (!a.white || !b.white || *a.white == *b.white) &&
           (!a.primaries_xy || !b.primaries_xy ||
            *a.primaries_xy == *b.primaries_xy) &&
           (!a.profile || !b.profile || *a.profile == *b.profile);
  };
  std::map<std::uint64_t, TensorChannelDescription> assertions;
  for (std::size_t i = 0; i < value.channels.size(); ++i)
    assertions[i] = value.channels[i];
  for (const auto& group : value.groups)
    for (std::size_t i = 0; i < group.indices.size(); ++i) {
      auto c = group.components[i];
      c.interpretation = group.interpretation;
      auto& old = assertions[group.indices[i]];
      if (!compatible_text(old.name, c.name) ||
          !compatible_text(old.role, c.role) ||
          !compatible_text(old.unit, c.unit) ||
          (old.interpretation &&
           !compatible_interpretation(*old.interpretation, *c.interpretation)))
        return invalid("group conflicts with channel or overlapping group");
      if (!c.name.empty())
        old.name = c.name;
      if (!c.role.empty())
        old.role = c.role;
      if (!c.unit.empty())
        old.unit = c.unit;
      old.interpretation = c.interpretation;
    }
  return Status::success();
}
void put_u16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}
void put_u64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void put_f64(std::vector<std::uint8_t>* bytes, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u64(bytes, bits);
}
void put_text(std::vector<std::uint8_t>* bytes, const std::string& value) {
  bytes->push_back(static_cast<std::uint8_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}
void put_interpretation(std::vector<std::uint8_t>* bytes,
                        const TensorInterpretation& v) {
  for (const auto* text :
       {&v.model, &v.primaries, &v.transfer, &v.reference, &v.association})
    put_text(bytes, *text);
  bytes->push_back(v.white ? 1 : 0);
  if (v.white)
    for (auto n : *v.white)
      put_f64(bytes, n);
  bytes->push_back(v.primaries_xy ? 1 : 0);
  if (v.primaries_xy)
    for (auto n : *v.primaries_xy)
      put_f64(bytes, n);
  bytes->push_back(v.profile ? 1 : 0);
  if (v.profile) {
    put_u64(bytes, v.profile->byte_length);
    bytes->insert(bytes->end(), v.profile->sha256.begin(),
                  v.profile->sha256.end());
  }
}
void put_channel(std::vector<std::uint8_t>* bytes,
                 const TensorChannelDescription& value) {
  put_text(bytes, value.name);
  put_text(bytes, value.role);
  put_text(bytes, value.unit);
  bytes->push_back(value.interpretation ? 1 : 0);
  if (value.interpretation)
    put_interpretation(bytes, *value.interpretation);
}
struct Reader final {
  const std::vector<std::uint8_t>& bytes;
  std::size_t offset = 0;
  bool byte(std::uint8_t* value) {
    if (offset == bytes.size())
      return false;
    *value = bytes[offset++];
    return true;
  }
  bool u16(std::uint16_t* value) {
    std::uint8_t low = 0, high = 0;
    if (!byte(&low) || !byte(&high))
      return false;
    *value = static_cast<std::uint16_t>(low | (high << 8));
    return true;
  }
  bool f64(double* value) {
    std::uint64_t bits = 0;
    if (!u64(&bits))
      return false;
    std::memcpy(value, &bits, sizeof(bits));
    return true;
  }
  bool u64(std::uint64_t* value) {
    if (bytes.size() - offset < 8)
      return false;
    *value = 0;
    for (unsigned i = 0; i < 8; ++i)
      *value |= static_cast<std::uint64_t>(bytes[offset++]) << (8 * i);
    return true;
  }
  bool text(std::string* value) {
    std::uint8_t size = 0;
    if (!byte(&size) || size > 128 || bytes.size() - offset < size)
      return false;
    value->assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
    offset += size;
    return valid_text(*value);
  }
  bool interpretation(TensorInterpretation* v) {
    for (auto* t : {&v->model, &v->primaries, &v->transfer, &v->reference,
                    &v->association})
      if (!text(t))
        return false;
    std::uint8_t present = 0;
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->white.emplace();
      for (auto& n : *v->white)
        if (!f64(&n))
          return false;
    }
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->primaries_xy.emplace();
      for (auto& n : *v->primaries_xy)
        if (!f64(&n))
          return false;
    }
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->profile.emplace();
      if (!u64(&v->profile->byte_length))
        return false;
      for (auto& n : v->profile->sha256)
        if (!byte(&n))
          return false;
    }
    return valid_interpretation(*v);
  }
  bool channel(TensorChannelDescription* value) {
    std::uint8_t present = 0;
    if (!text(&value->name) || !text(&value->role) || !text(&value->unit) ||
        !byte(&present) || present > 1)
      return false;
    if (present) {
      value->interpretation.emplace();
      if (!interpretation(&*value->interpretation))
        return false;
    }
    return true;
  }
};
}  // namespace

Result<ValueFacet> encode_tensor_description(
    const TensorDescription& description) {
  auto status = validate_structure(description);
  if (!status.ok())
    return Result<ValueFacet>(status);
  ValueFacet facet;
  facet.key = kKey;
  facet.version = 2;
  auto& bytes = facet.payload;
  bytes.insert(bytes.end(), {'T', 'D', 'M', '2'});
  bytes.push_back(description.channel_axis
                      ? static_cast<std::uint8_t>(*description.channel_axis)
                      : 255);
  put_u16(&bytes, static_cast<std::uint16_t>(description.channels.size()));
  for (const auto& channel : description.channels)
    put_channel(&bytes, channel);
  bytes.push_back(description.component ? 1 : 0);
  if (description.component)
    put_channel(&bytes, *description.component);
  bytes.push_back(static_cast<std::uint8_t>(description.axes.size()));
  for (const auto& axis : description.axes) {
    put_text(&bytes, axis.name);
    put_text(&bytes, axis.unit);
    put_f64(&bytes, axis.origin);
    put_f64(&bytes, axis.step);
  }
  for (const auto* field :
       {&description.model, &description.primaries, &description.transfer,
        &description.reference, &description.association})
    put_text(&bytes, *field);
  bytes.push_back(description.white ? 1 : 0);
  if (description.white)
    for (const auto number : *description.white)
      put_f64(&bytes, number);
  bytes.push_back(description.primaries_xy ? 1 : 0);
  if (description.primaries_xy)
    for (const auto number : *description.primaries_xy)
      put_f64(&bytes, number);
  bytes.push_back(description.profile ? 1 : 0);
  if (description.profile) {
    put_u64(&bytes, description.profile->byte_length);
    bytes.insert(bytes.end(), description.profile->sha256.begin(),
                 description.profile->sha256.end());
  }
  put_u16(&bytes, static_cast<std::uint16_t>(description.groups.size()));
  for (const auto& group : description.groups) {
    put_text(&bytes, group.name);
    put_u16(&bytes, static_cast<std::uint16_t>(group.indices.size()));
    for (std::size_t i = 0; i < group.indices.size(); ++i) {
      put_u64(&bytes, group.indices[i]);
      put_channel(&bytes, group.components[i]);
    }
    put_interpretation(&bytes, group.interpretation);
    bytes.push_back(group.alpha ? 1 : 0);
    if (group.alpha)
      put_u64(&bytes, *group.alpha);
  }
  if (bytes.size() > 4096)
    return Result<ValueFacet>(
        invalid("tensor description exceeds facet bound"));
  return Result<ValueFacet>(std::move(facet));
}

Result<TensorDescription> decode_tensor_description(const ValueFacet& facet) {
  using Answer = Result<TensorDescription>;
  if (facet.key != kKey || facet.version != 2 || facet.payload.size() < 9 ||
      facet.payload.size() > 4096 || facet.payload[0] != 'T' ||
      facet.payload[1] != 'D' || facet.payload[2] != 'M' ||
      facet.payload[3] != '2')
    return Answer(invalid("invalid tensor description facet"));
  Reader reader{facet.payload, 4};
  TensorDescription value;
  std::uint8_t axis = 0, has_component = 0, axes = 0;
  std::uint16_t count = 0;
  if (!reader.byte(&axis) || (axis != 255 && axis >= 8) || !reader.u16(&count))
    return Answer(invalid("invalid tensor description header"));
  if (axis != 255)
    value.channel_axis = axis;
  if (count > (facet.payload.size() - reader.offset) / 3)
    return Answer(invalid("truncated tensor channel table"));
  value.channels.resize(count);
  for (auto& channel : value.channels)
    if (!reader.channel(&channel))
      return Answer(invalid("invalid tensor channel table"));
  if (!reader.byte(&has_component) || has_component > 1)
    return Answer(invalid("invalid tensor component marker"));
  if (has_component) {
    value.component.emplace();
    if (!reader.channel(&*value.component))
      return Answer(invalid("invalid tensor component"));
  }
  if (!reader.byte(&axes) || axes > 8)
    return Answer(invalid("invalid tensor axis count"));
  value.axes.resize(axes);
  for (auto& described : value.axes)
    if (!reader.text(&described.name) || !reader.text(&described.unit) ||
        !reader.f64(&described.origin) || !reader.f64(&described.step))
      return Answer(invalid("invalid tensor axis data"));
  for (auto* field : {&value.model, &value.primaries, &value.transfer,
                      &value.reference, &value.association})
    if (!reader.text(field))
      return Answer(invalid("invalid tensor interpretation"));
  std::uint8_t present = 0;
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor white marker"));
  if (present) {
    value.white.emplace();
    for (auto& number : *value.white)
      if (!reader.f64(&number))
        return Answer(invalid("truncated tensor white"));
  }
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor primaries marker"));
  if (present) {
    value.primaries_xy.emplace();
    for (auto& number : *value.primaries_xy)
      if (!reader.f64(&number))
        return Answer(invalid("truncated tensor primaries"));
  }
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor profile marker"));
  if (present) {
    value.profile.emplace();
    if (!reader.u64(&value.profile->byte_length) ||
        facet.payload.size() - reader.offset < value.profile->sha256.size())
      return Answer(invalid("truncated tensor profile identity"));
    for (auto& byte : value.profile->sha256)
      if (!reader.byte(&byte))
        return Answer(invalid("truncated tensor profile identity"));
  }
  std::uint16_t group_count = 0;
  if (!reader.u16(&group_count) || group_count > 128)
    return Answer(invalid("invalid group count"));
  value.groups.resize(group_count);
  for (auto& group : value.groups) {
    std::uint16_t count = 0;
    if (!reader.text(&group.name) || !reader.u16(&count) || count > 64)
      return Answer(invalid("invalid group header"));
    group.indices.resize(count);
    group.components.resize(count);
    for (std::size_t i = 0; i < count; ++i)
      if (!reader.u64(&group.indices[i]) ||
          !reader.channel(&group.components[i]))
        return Answer(invalid("invalid group components"));
    if (!reader.interpretation(&group.interpretation) ||
        !reader.byte(&present) || present > 1)
      return Answer(invalid("invalid group interpretation"));
    if (present) {
      group.alpha.emplace();
      if (!reader.u64(&*group.alpha))
        return Answer(invalid("invalid group alpha"));
    }
  }
  if (reader.offset != facet.payload.size() || !validate_structure(value).ok())
    return Answer(invalid("noncanonical tensor description"));
  auto canonical = encode_tensor_description(value);
  if (!canonical.ok() || canonical.value().payload != facet.payload)
    return Answer(invalid("noncanonical tensor description bytes"));
  return Answer(std::move(value));
}

Result<std::string> tensor_description_parameter(
    const TensorDescription& description) {
  auto encoded = encode_tensor_description(description);
  if (!encoded.ok())
    return Result<std::string>(encoded.status());
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(encoded.value().payload.size() * 2);
  for (const auto byte : encoded.value().payload) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return Result<std::string>(std::move(result));
}

Result<TensorDescription> tensor_description_from_parameter(
    const std::string& parameter) {
  if (parameter.size() < 18 || parameter.size() > 8192 || parameter.size() % 2)
    return Result<TensorDescription>(invalid("invalid tensor override length"));
  ValueFacet facet;
  facet.key = kKey;
  facet.version = 2;
  facet.payload.reserve(parameter.size() / 2);
  const auto nibble = [](char digit) -> int {
    if (digit >= '0' && digit <= '9')
      return digit - '0';
    if (digit >= 'a' && digit <= 'f')
      return digit - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < parameter.size(); i += 2) {
    const auto high = nibble(parameter[i]), low = nibble(parameter[i + 1]);
    if (high < 0 || low < 0)
      return Result<TensorDescription>(invalid("invalid tensor override hex"));
    facet.payload.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return decode_tensor_description(facet);
}

Status validate_tensor_description(const TensorDescription& description,
                                   const ValueDescriptor& descriptor) {
  auto status = validate_structure(description);
  if (!status.ok())
    return status;
  const auto rank = descriptor.shape.size();
  if (rank == 0 || rank > 8 ||
      (description.channel_axis && *description.channel_axis >= rank) ||
      (!description.axes.empty() && description.axes.size() != rank))
    return invalid("tensor description disagrees with shape");
  if (!description.channels.empty() &&
      description.channels.size() !=
          descriptor.shape[*description.channel_axis])
    return {ErrorCode::TypeMismatch,
            "tensor channel table length disagrees with shape",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}};
  std::set<std::string> names;
  for (const auto& group : description.groups) {
    if (!description.channel_axis || !names.insert(group.name).second)
      return invalid("group requires channel axis and unique name");
    const auto count = descriptor.shape[*description.channel_axis];
    for (auto index : group.indices)
      if (index >= count)
        return invalid("group index exceeds channel count");
    if (group.alpha && *group.alpha >= count)
      return invalid("group alpha exceeds channel count");
  }
  return Status::success();
}
}  // namespace ps
