#include "photospider/data/tensor_description.hpp"

#include <cmath>
#include <cstring>
#include <limits>
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
bool valid_channel(const TensorChannelDescription& channel) {
  return valid_text(channel.name) && valid_text(channel.role) &&
         valid_text(channel.unit);
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
void put_channel(std::vector<std::uint8_t>* bytes,
                 const TensorChannelDescription& value) {
  put_text(bytes, value.name);
  put_text(bytes, value.role);
  put_text(bytes, value.unit);
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
  bool channel(TensorChannelDescription* value) {
    return text(&value->name) && text(&value->role) && text(&value->unit);
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
  facet.version = 1;
  auto& bytes = facet.payload;
  bytes.insert(bytes.end(), {'T', 'D', 'M', '1'});
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
  if (bytes.size() > 4096)
    return Result<ValueFacet>(
        invalid("tensor description exceeds facet bound"));
  return Result<ValueFacet>(std::move(facet));
}

Result<TensorDescription> decode_tensor_description(const ValueFacet& facet) {
  using Answer = Result<TensorDescription>;
  if (facet.key != kKey || facet.version != 1 || facet.payload.size() < 9 ||
      facet.payload.size() > 4096 || facet.payload[0] != 'T' ||
      facet.payload[1] != 'D' || facet.payload[2] != 'M' ||
      facet.payload[3] != '1')
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
  facet.version = 1;
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
  return Status::success();
}
}  // namespace ps
