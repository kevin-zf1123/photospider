#include "photospider/data/semantic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "plugin/utf8_validation.hpp"

namespace ps {
namespace {
constexpr std::array<const char*, 10> kinds = {
    "",
    "scalar",
    "image",
    "mask",
    "scalar_field",
    "vector_field",
    "complex_field",
    "sampled_signal",
    "lut",
    "byte_resource",
};
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
bool one_of(const std::string& text,
            std::initializer_list<const char*> options) {
  return std::any_of(options.begin(), options.end(),
                     [&](const char* value) { return text == value; });
}
bool text_valid(const std::string& text) {
  return text.size() <= 128 &&
         (text.empty() || plugin_internal::valid_utf8_key(text));
}
Status validate(const SemanticDescriptor& s) {
  const auto kind = static_cast<std::uint32_t>(s.kind);
  if (kind == 0 || kind > 9 || s.channels.size() > 64)
    return invalid("invalid semantic kind/channel count");
  for (const auto* text : {&s.model, &s.primaries, &s.transfer, &s.reference,
                           &s.unit, &s.association, &s.coordinate_space,
                           &s.direction, &s.sample_axis_unit, &s.media_type})
    if (!text_valid(*text))
      return invalid("invalid semantic text");
  std::set<std::string> names;
  for (const auto& c : s.channels)
    if (!text_valid(c.name) || c.name.empty() || !text_valid(c.role) ||
        c.role.empty() || !text_valid(c.unit) || c.unit.empty() ||
        !names.insert(c.name).second)
      return invalid("invalid or duplicate semantic channel");
  if (s.unit.empty())
    return invalid("semantic value unit is required");
  const bool image = s.kind == SemanticKind::Image;
  const bool sampled =
      s.kind == SemanticKind::SampledSignal || s.kind == SemanticKind::Lut;
  if (!std::isfinite(s.sample_origin) || !std::isfinite(s.sample_step) ||
      (sampled ? s.sample_step <= 0 || s.sample_axis_unit.empty()
               : s.sample_origin != 0 || s.sample_step != 0 ||
                     !s.sample_axis_unit.empty()))
    return invalid("invalid sampling domain");
  if (image) {
    if (!one_of(s.model, {"rgb", "xyz", "lab"}) ||
        !one_of(s.association,
                {"none", "straight", "coverage_premultiplied"}) ||
        !one_of(s.reference, {"scene", "display"}) || s.white[1] != 1 ||
        std::any_of(s.white.begin(), s.white.end(),
                    [](double x) { return !std::isfinite(x) || x <= 0; }) ||
        (s.model == "rgb" ? s.primaries != "srgb" : !s.primaries.empty()) ||
        (s.model == "lab" ? s.transfer != "identity"
                          : s.transfer != "linear") ||
        (s.association == "coverage_premultiplied" && s.model != "rgb"))
      return invalid("invalid image color/alpha semantics");
    const bool alpha = s.association != "none";
    if (s.channels.size() != (alpha ? 4U : 3U))
      return invalid("image channel count contradicts association");
    const std::vector<std::string> roles =
        s.model == "rgb"   ? std::vector<std::string>{"red", "green", "blue"}
        : s.model == "xyz" ? std::vector<std::string>{"x", "y", "z"}
                           : std::vector<std::string>{"lightness", "a", "b"};
    std::set<std::string> seen_roles;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto& role = s.channels[i].role;
      const auto unit =
          s.model == "lab"
              ? (role == "lightness" ? "lab_lightness" : "lab_opponent")
              : "relative";
      if (std::find(roles.begin(), roles.end(), role) == roles.end() ||
          !seen_roles.insert(role).second || s.channels[i].unit != unit)
        return invalid("image channel role/unit mismatch");
    }
    if (s.unit != (s.model == "lab" ? "lab" : "relative") ||
        (alpha && (s.channels[3].role != "coverage" ||
                   s.channels[3].unit != "dimensionless")))
      return invalid("image unit/coverage mismatch");
  } else if (!s.model.empty() || !s.primaries.empty() || !s.transfer.empty() ||
             !s.reference.empty() || !s.association.empty() ||
             std::any_of(s.white.begin(), s.white.end(), [](double x) {
               return !std::isfinite(x) || x != 0;
             })) {
    return invalid("color fields require image semantics");
  }
  if (s.kind == SemanticKind::ByteResource) {
    if (s.media_type.empty() || !s.channels.empty())
      return invalid("byte resource requires media type and no channels");
  } else if (!s.media_type.empty() || s.channels.empty()) {
    return invalid("typed samples require channels and no media type");
  }
  if ((s.kind == SemanticKind::Scalar || s.kind == SemanticKind::ScalarField ||
       s.kind == SemanticKind::Mask) &&
      s.channels.size() != 1)
    return invalid("scalar/field/mask requires one channel");
  if (s.kind == SemanticKind::Mask &&
      (!one_of(s.channels[0].role, {"coverage", "probability", "membership"}) ||
       s.channels[0].unit != "dimensionless" || s.unit != "dimensionless"))
    return invalid("mask role/unit mismatch");
  const bool vector = s.kind == SemanticKind::VectorField;
  const bool complex = s.kind == SemanticKind::ComplexField;
  if (vector && (s.channels.size() < 2 || s.channels.size() > 3 ||
                 !one_of(s.coordinate_space,
                         {"pixel_displacement", "normalized_displacement",
                          "pixel_position", "normalized_position"}) ||
                 !one_of(s.direction, {"forward", "inverse"})))
    return invalid("vector requires coordinates and direction");
  if (complex && (s.channels.size() != 2 || s.channels[0].role != "real" ||
                  s.channels[1].role != "imaginary" ||
                  s.coordinate_space != "frequency_unshifted" ||
                  s.direction != "forward_negative_inverse_1n"))
    return invalid("complex field requires real/imaginary channels");
  if (!vector && !complex &&
      (!s.direction.empty() || !s.coordinate_space.empty()))
    return invalid("coordinate fields require vector semantics");
  if (!image && s.kind != SemanticKind::ByteResource)
    for (const auto& channel : s.channels)
      if (channel.unit != s.unit)
        return invalid("sample unit differs from channel unit");
  return Status::success();
}
void integer(std::vector<std::uint8_t>* out, std::uint64_t value, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    out->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void string(std::vector<std::uint8_t>* out, const std::string& text) {
  integer(out, text.size(), 4);
  out->insert(out->end(), text.begin(), text.end());
}
void number(std::vector<std::uint8_t>* out, double value) {
  std::uint64_t bits = 0;
  if (value != 0)
    std::memcpy(&bits, &value, 8);
  integer(out, bits, 8);
}
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
  std::uint64_t integer(unsigned n) {
    if (n > bytes_.size() - position_) {
      ok = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
      value |= static_cast<std::uint64_t>(bytes_[position_++]) << (8 * i);
    return value;
  }
  std::string string() {
    auto n = integer(4);
    if (!ok || n > 128 || n > bytes_.size() - position_) {
      ok = false;
      return {};
    }
    std::string result(bytes_.begin() + position_,
                       bytes_.begin() + position_ + n);
    position_ += n;
    return result;
  }
  double number() {
    const auto bits = integer(8);
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
  }
  bool complete() const { return ok && position_ == bytes_.size(); }
  bool ok = true;

 private:
  const std::vector<std::uint8_t>& bytes_;
  std::size_t position_ = 0;
};
}  // namespace

SemanticDescriptor rgba_semantics() {
  SemanticDescriptor s;
  s.kind = SemanticKind::Image;
  s.channels = {{"R", "red", "relative"},
                {"G", "green", "relative"},
                {"B", "blue", "relative"},
                {"A", "coverage", "dimensionless"}};
  s.model = "rgb";
  s.primaries = "srgb";
  s.white = {0.3127 / 0.3290, 1, (1 - 0.3127 - 0.3290) / 0.3290};
  s.transfer = "linear";
  s.reference = "scene";
  s.unit = "relative";
  s.association = "coverage_premultiplied";
  return s;
}
SemanticDescriptor coverage_semantics() {
  SemanticDescriptor s;
  s.kind = SemanticKind::Mask;
  s.channels = {{"coverage", "coverage", "dimensionless"}};
  return s;
}
Result<ValueFacet> encode_semantic(const SemanticDescriptor& s) {
  auto status = validate(s);
  if (!status.ok())
    return Result<ValueFacet>(status);
  ValueFacet facet{s.kind == SemanticKind::Image ? "photospider.image"
                                                 : "photospider.semantic",
                   s.kind == SemanticKind::Image ? 2U : 1U,
                   {}};
  auto* out = &facet.payload;
  string(out, kinds[static_cast<std::uint32_t>(s.kind)]);
  integer(out, s.channels.size(), 4);
  for (const auto& c : s.channels) {
    string(out, c.name);
    string(out, c.role);
    string(out, c.unit);
  }
  string(out, s.model);
  string(out, s.primaries);
  for (auto x : s.white)
    number(out, x);
  for (const auto* text : {&s.transfer, &s.reference, &s.unit, &s.association,
                           &s.coordinate_space, &s.direction})
    string(out, *text);
  number(out, s.sample_origin);
  number(out, s.sample_step);
  string(out, s.sample_axis_unit);
  string(out, s.media_type);
  if (out->size() > 4096)
    return Result<ValueFacet>(invalid("semantic payload exceeds 4096 bytes"));
  return Result<ValueFacet>(std::move(facet));
}
Result<SemanticDescriptor> decode_semantic(const ValueFacet& facet) {
  if (facet.payload.size() > 4096 ||
      !((facet.key == "photospider.image" && facet.version == 2) ||
        (facet.key == "photospider.semantic" && facet.version == 1)))
    return Result<SemanticDescriptor>(invalid("unsupported semantic facet"));
  Reader r(facet.payload);
  const auto kind = r.string();
  SemanticDescriptor s;
  s.kind = static_cast<SemanticKind>(0);
  for (std::uint32_t i = 1; i <= 9; ++i)
    if (kind == kinds[i])
      s.kind = static_cast<SemanticKind>(i);
  const auto count = r.integer(4);
  if (!r.ok || count > 64)
    return Result<SemanticDescriptor>(
        invalid("invalid semantic channel count"));
  for (std::uint64_t i = 0; i < count; ++i)
    s.channels.push_back({r.string(), r.string(), r.string()});
  s.model = r.string();
  s.primaries = r.string();
  for (auto& x : s.white)
    x = r.number();
  s.transfer = r.string();
  s.reference = r.string();
  s.unit = r.string();
  s.association = r.string();
  s.coordinate_space = r.string();
  s.direction = r.string();
  s.sample_origin = r.number();
  s.sample_step = r.number();
  s.sample_axis_unit = r.string();
  s.media_type = r.string();
  if (!r.complete())
    return Result<SemanticDescriptor>(
        invalid("truncated or trailing semantic bytes"));
  auto encoded = encode_semantic(s);
  if (!encoded.ok())
    return Result<SemanticDescriptor>(encoded.status());
  if (encoded.value().key != facet.key ||
      encoded.value().payload != facet.payload)
    return Result<SemanticDescriptor>(invalid("noncanonical semantic payload"));
  return Result<SemanticDescriptor>(std::move(s));
}
Result<std::string> semantic_parameter(const SemanticDescriptor& s) {
  auto facet = encode_semantic(s);
  if (!facet.ok())
    return Result<std::string>(facet.status());
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto byte : facet.value().payload) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return Result<std::string>(std::move(result));
}
Result<SemanticDescriptor> semantic_from_parameter(
    const std::string& parameter) {
  if (parameter.empty() || parameter.size() > 8192 || parameter.size() % 2)
    return Result<SemanticDescriptor>(
        invalid("invalid semantic parameter size"));
  ValueFacet facet{"photospider.semantic", 1, {}};
  const auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
  };
  for (std::size_t i = 0; i < parameter.size(); i += 2) {
    const int a = digit(parameter[i]), b = digit(parameter[i + 1]);
    if (a < 0 || b < 0)
      return Result<SemanticDescriptor>(invalid("noncanonical semantic hex"));
    facet.payload.push_back(static_cast<std::uint8_t>(a * 16 + b));
  }
  Reader r(facet.payload);
  if (r.string() == "image") {
    facet.key = "photospider.image";
    facet.version = 2;
  }
  return decode_semantic(facet);
}
Result<std::string> channel_indices_parameter(
    const std::vector<std::uint32_t>& indices) {
  if (indices.empty() || indices.size() > 64)
    return Result<std::string>(invalid("channel index count outside [1,64]"));
  std::string result;
  for (auto index : indices) {
    if (index > 63)
      return Result<std::string>(invalid("channel index outside [0,63]"));
    if (!result.empty())
      result += ',';
    result += std::to_string(index);
  }
  return Result<std::string>(std::move(result));
}
Result<std::vector<std::uint32_t>> channel_indices_from_parameter(
    const std::string& parameter) {
  if (parameter.empty() || parameter.size() > 191)
    return Result<std::vector<std::uint32_t>>(
        invalid("invalid channel index String size"));
  std::vector<std::uint32_t> result;
  std::size_t position = 0;
  while (position < parameter.size()) {
    const auto start = position;
    std::uint32_t value = 0;
    while (position < parameter.size() && parameter[position] >= '0' &&
           parameter[position] <= '9') {
      value = value * 10 + static_cast<unsigned>(parameter[position++] - '0');
      if (value > 63 || (position - start > 1 && parameter[start] == '0'))
        return Result<std::vector<std::uint32_t>>(
            invalid("noncanonical channel index"));
    }
    if (position == start || result.size() == 64 ||
        (position < parameter.size() &&
         (parameter[position] != ',' || position + 1 == parameter.size())))
      return Result<std::vector<std::uint32_t>>(
          invalid("invalid channel index list"));
    result.push_back(value);
    if (position < parameter.size())
      ++position;
  }
  return Result<std::vector<std::uint32_t>>(std::move(result));
}
Status validate_semantic_descriptor(const SemanticDescriptor& s,
                                    const ValueDescriptor& d) {
  auto encoded = encode_semantic(s);
  if (!encoded.ok())
    return encoded.status();
  const auto mismatch = [] {
    return Status::failure(ErrorCode::TypeMismatch,
                           "semantic dtype/shape mismatch");
  };
  if (d.shape.empty() || d.shape.size() > 8 ||
      std::any_of(d.shape.begin(), d.shape.end(),
                  [](auto n) { return n == 0; }))
    return mismatch();
  const bool floating = d.element_type == ElementType::Float32 ||
                        d.element_type == ElementType::Float64;
  if (s.kind == SemanticKind::ByteResource)
    return d.element_type == ElementType::UInt8 && d.shape.size() == 1
               ? Status::success()
               : mismatch();
  if (!floating && d.element_type != ElementType::Int64)
    return mismatch();
  switch (s.kind) {
    case SemanticKind::Image:
      if (d.element_type != ElementType::Float32 || d.shape.size() != 3 ||
          d.shape[2] != s.channels.size())
        return mismatch();
      break;
    case SemanticKind::Mask:
      if (d.element_type != ElementType::Float32 || d.shape.size() != 2)
        return mismatch();
      break;
    case SemanticKind::Scalar:
      if (d.shape != std::vector<std::uint64_t>{1})
        return mismatch();
      break;
    case SemanticKind::ScalarField:
      if (d.shape.size() != 2)
        return mismatch();
      break;
    case SemanticKind::VectorField:
    case SemanticKind::ComplexField:
      if (!floating || d.shape.size() != 3 || d.shape[2] != s.channels.size())
        return mismatch();
      break;
    case SemanticKind::SampledSignal:
    case SemanticKind::Lut:
      if (!floating ||
          !((d.shape.size() == 1 && s.channels.size() == 1) ||
            (d.shape.size() == 2 && d.shape[1] == s.channels.size())))
        return mismatch();
      break;
    case SemanticKind::ByteResource:
      break;
  }
  return Status::success();
}
Status validate_semantic_value(const SemanticDescriptor& s, const Value& value,
                               ErrorCode failure,
                               const std::function<ErrorCode()>& stop) {
  if (!value.valid())
    return invalid("invalid semantic Value");
  // Float32-to-Float64 conversion can flush subnormals on the caller's thread.
  // Validate exact stored samples under the same environment as image ports.
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Status::failure(ErrorCode::OperationFailed,
                           "cannot set typed sample numeric environment");
  auto status = validate_semantic_descriptor(s, value.descriptor());
  if (!status.ok())
    return status;
  if (value.region().empty())
    return Status::failure(ErrorCode::TypeMismatch, "empty typed coverage");
  if (s.kind == SemanticKind::ByteResource)
    return Status::success();
  const auto& dims = value.region().dimensions();
  const bool image = s.kind == SemanticKind::Image;
  if (image && (dims[2].offset != 0 || dims[2].extent != s.channels.size()))
    return Status::failure(ErrorCode::TypeMismatch,
                           "image coverage omits channels");
  std::vector<std::uint64_t> index;
  for (auto d : dims)
    index.push_back(d.offset);
  std::uint64_t visited = 0;
  double color[3] = {};
  for (;;) {
    if ((visited++ & 1023U) == 0 && stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        Status result;
        result.code = code;
        return result;
      }
    }
    auto address = value.byte_address(index);
    if (!address.ok())
      return address.status();
    double sample = 0;
    const auto* data = value.bytes().data() + address.value();
    if (value.descriptor().element_type == ElementType::Float32) {
      float x;
      std::memcpy(&x, data, 4);
      sample = x;
    } else if (value.descriptor().element_type == ElementType::Float64) {
      std::memcpy(&sample, data, 8);
    } else {
      std::int64_t x;
      std::memcpy(&x, data, 8);
      sample = static_cast<double>(x);
    }
    bool valid = std::isfinite(sample);
    if (s.kind == SemanticKind::Mask)
      valid = valid && sample >= 0 && sample <= 1;
    if (image) {
      const auto c = index[2];
      if (c < 3)
        color[c] = sample;
      else
        valid = valid && sample >= 0 && sample <= 1 &&
                (s.association != "coverage_premultiplied" || sample != 0 ||
                 (color[0] == 0 && color[1] == 0 && color[2] == 0));
    }
    if (!valid)
      return Status::failure(failure, "sample violates typed semantic domain");
    std::size_t axis = index.size();
    while (axis) {
      --axis;
      if (++index[axis] < dims[axis].offset + dims[axis].extent)
        break;
      index[axis] = dims[axis].offset;
    }
    if (axis == 0 && index[0] == dims[0].offset)
      break;
  }
  return Status::success();
}
}  // namespace ps
