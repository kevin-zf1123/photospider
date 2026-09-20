#include "photospider/data/color_array.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "data/color_array_exact.hpp"
#include "data/input_validation.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain};
}
std::uint64_t bits(double value) {
  std::uint64_t result;
  std::memcpy(&result, &value, 8);
  return result;
}
bool finite(double value) {
  return ((bits(value) >> 52) & 2047) != 2047;
}
bool positive(double value) {
  const auto raw = bits(value);
  return finite(value) && !(raw >> 63) && raw != 0;
}
bool has_rgb(ColorModel model) {
  return model == ColorModel::Rgb || model == ColorModel::Hsl ||
         model == ColorModel::Ycbcr;
}
bool polar(ColorModel model) {
  return model == ColorModel::Cielch || model == ColorModel::Oklch ||
         model == ColorModel::Hsl;
}
Status validate(const ColorArrayDescriptor& s, bool static_source) {
  const auto model = static_cast<unsigned>(s.model);
  if (model < 1 || model > 9 || static_cast<unsigned>(s.association) > 2 ||
      (s.model != ColorModel::Rgb && s.association != ColorAssociation::None))
    return invalid("invalid color model/association");
  const bool cmyk = s.model == ColorModel::Cmyk;
  if (cmyk ? s.reference != ColorReference::ProfileRelative
           : s.reference != ColorReference::SceneRelative &&
                 s.reference != ColorReference::DisplayRelative)
    return invalid("invalid color reference");
  const bool split = s.source_layout == ColorSourceLayout::RationalHueSplit;
  if ((s.source_layout != ColorSourceLayout::Interleaved && !split) ||
      (split && (!static_source || !polar(s.model))))
    return invalid("invalid runtime/static color layout");
  if (s.hue.has_value() != polar(s.model) ||
      (s.hue && (split ? *s.hue != ColorHueUnit::RationalPi
                       : *s.hue != ColorHueUnit::Radian &&
                             *s.hue != ColorHueUnit::PiMultiple)))
    return invalid("invalid color hue unit");
  if (s.white.has_value() == cmyk || s.profile.has_value() != cmyk ||
      s.primaries.has_value() != has_rgb(s.model) ||
      s.transfer.has_value() != has_rgb(s.model) ||
      s.ncl_coefficients.has_value() != (s.model == ColorModel::Ycbcr))
    return invalid("missing or irrelevant color fields");
  if (cmyk)
    return s.profile->byte_length >= 132
               ? Status::success()
               : invalid(
                     "ICC profile length is smaller than its header/directory");
  const auto& white = *s.white;
  if (!finite(white[0]) || !finite(white[1]) ||
      !color_internal::positive_sum_below_one(white[0], white[1]))
    return invalid("invalid color white xy");
  if ((s.model == ColorModel::Oklab || s.model == ColorModel::Oklch) &&
      (bits(white[0]) != bits(.3127) || bits(white[1]) != bits(.3290)))
    return invalid("OK coordinates require D65");
  if (s.primaries) {
    if (!std::all_of(s.primaries->begin(), s.primaries->end(), finite) ||
        !color_internal::valid_basis(*s.primaries, white))
      return invalid("singular color primary basis or white normalization");
    const auto& transfer = *s.transfer;
    const bool gamma = transfer.kind == ColorTransferKind::Gamma;
    if (static_cast<unsigned>(transfer.kind) > 2 ||
        transfer.gamma.has_value() != gamma ||
        (gamma && !positive(*transfer.gamma)))
      return invalid("invalid color transfer/exponent");
  }
  if (s.ncl_coefficients) {
    const auto& ncl = *s.ncl_coefficients;
    if (!finite(ncl[0]) || !finite(ncl[1]) ||
        !color_internal::positive_sum_below_one(ncl[0], ncl[1]))
      return invalid("invalid NCL Kr/Kb");
  }
  return Status::success();
}
void integer(std::vector<std::uint8_t>* out, std::uint64_t value,
             unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    out->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void number(std::vector<std::uint8_t>* out, double value) {
  auto raw = bits(value);
  if (!(raw & 0x7fffffffffffffffULL))
    raw = 0;
  integer(out, raw, 8);
}
Result<ValueFacet> encode(const ColorArrayDescriptor& s, bool static_source) {
  auto status = validate(s, static_source);
  if (!status.ok())
    return Result<ValueFacet>(status);
  ValueFacet facet{"photospider.color-array", 1, {}};
  auto* out = &facet.payload;
  out->reserve(94);  // Largest v1 record: gamma RGB fields plus NCL pair.
  integer(out, static_cast<unsigned>(s.model), 1);
  integer(out, static_cast<unsigned>(s.reference), 1);
  integer(out, static_cast<unsigned>(s.association), 1);
  integer(out, static_cast<unsigned>(s.source_layout), 1);
  if (s.profile) {
    integer(out, s.profile->byte_length, 8);
    out->insert(out->end(), s.profile->sha256.begin(), s.profile->sha256.end());
  } else {
    for (auto value : *s.white)
      number(out, value);
    if (s.primaries) {
      for (auto value : *s.primaries)
        number(out, value);
      integer(out, static_cast<unsigned>(s.transfer->kind), 1);
      if (s.transfer->gamma)
        number(out, *s.transfer->gamma);
    }
    if (s.hue)
      integer(out, static_cast<unsigned>(*s.hue), 1);
    if (s.ncl_coefficients)
      for (auto value : *s.ncl_coefficients)
        number(out, value);
  }
  return Result<ValueFacet>(std::move(facet));
}
class Reader final {
 public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
  std::uint64_t integer(unsigned size) {
    if (size > bytes_.size() - position_) {
      ok_ = false;
      return 0;
    }
    std::uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i)
      result |= static_cast<std::uint64_t>(bytes_[position_++]) << (8 * i);
    return result;
  }
  double number() {
    const auto raw = integer(8);
    double result;
    std::memcpy(&result, &raw, 8);
    return result;
  }
  bool complete() const { return ok_ && position_ == bytes_.size(); }

 private:
  const std::vector<std::uint8_t>& bytes_;
  std::size_t position_ = 0;
  bool ok_ = true;
};
Result<ColorArrayDescriptor> decode(const ValueFacet& facet,
                                    bool static_source) {
  if (facet.key != "photospider.color-array" || facet.version != 1 ||
      facet.payload.size() > 4096)
    return Result<ColorArrayDescriptor>(
        invalid("unsupported color-array facet"));
  Reader reader(facet.payload);
  ColorArrayDescriptor s;
  s.model = static_cast<ColorModel>(reader.integer(1));
  s.reference = static_cast<ColorReference>(reader.integer(1));
  s.association = static_cast<ColorAssociation>(reader.integer(1));
  s.source_layout = static_cast<ColorSourceLayout>(reader.integer(1));
  if (s.model == ColorModel::Cmyk) {
    s.white.reset();
    s.profile.emplace();
    s.profile->byte_length = reader.integer(8);
    for (auto& byte : s.profile->sha256)
      byte = static_cast<std::uint8_t>(reader.integer(1));
  } else {
    s.white = std::array<double, 2>{reader.number(), reader.number()};
    if (has_rgb(s.model)) {
      s.primaries.emplace();
      for (auto& value : *s.primaries)
        value = reader.number();
      s.transfer.emplace();
      s.transfer->kind = static_cast<ColorTransferKind>(reader.integer(1));
      if (s.transfer->kind == ColorTransferKind::Gamma)
        s.transfer->gamma = reader.number();
    }
    if (polar(s.model))
      s.hue = static_cast<ColorHueUnit>(reader.integer(1));
    if (s.model == ColorModel::Ycbcr)
      s.ncl_coefficients =
          std::array<double, 2>{reader.number(), reader.number()};
  }
  if (!reader.complete())
    return Result<ColorArrayDescriptor>(
        invalid("truncated or trailing color bytes"));
  auto canonical = encode(s, static_source);
  if (!canonical.ok())
    return Result<ColorArrayDescriptor>(canonical.status());
  if (canonical.value().payload != facet.payload)
    return Result<ColorArrayDescriptor>(invalid("noncanonical color bytes"));
  return Result<ColorArrayDescriptor>(std::move(s));
}
unsigned channels(const ColorArrayDescriptor& s) {
  return s.model == ColorModel::Cmyk ||
                 (s.model == ColorModel::Rgb &&
                  s.association != ColorAssociation::None)
             ? 4
             : 3;
}
}  // namespace

std::array<double, 2> color_white_d65() noexcept {
  return {.3127, .3290};
}
std::array<double, 2> color_white_d50() noexcept {
  return {.3457, .3585};
}
Result<ColorPrimaryCoordinates> color_primary_coordinates(
    ColorPrimaryPreset preset) {
  switch (preset) {
    case ColorPrimaryPreset::Srgb:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.64, .33, .30, .60, .15, .06},
                                  color_white_d65()});
    case ColorPrimaryPreset::DisplayP3:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.68, .32, .265, .69, .15, .06},
                                  color_white_d65()});
    case ColorPrimaryPreset::Rec2020:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.708, .292, .170, .797, .131, .046},
                                  color_white_d65()});
    case ColorPrimaryPreset::AdobeRgb1998:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.64, .33, .21, .71, .15, .06},
                                  color_white_d65()});
    case ColorPrimaryPreset::ProphotoRgb:
      return Result<ColorPrimaryCoordinates>(ColorPrimaryCoordinates{
          {.734699, .265301, .159597, .840403, .036598, .000105},
          color_white_d50()});
    case ColorPrimaryPreset::AcesAp0:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.73470, .26530, 0, 1, .00010, -.077},
                                  {.32168, .33767}});
    case ColorPrimaryPreset::AcesAp1:
      return Result<ColorPrimaryCoordinates>(
          ColorPrimaryCoordinates{{.713, .293, .165, .830, .128, .044},
                                  {.32168, .33767}});
  }
  return Result<ColorPrimaryCoordinates>(invalid("unknown primary preset"));
}
Result<std::array<double, 2>> color_ncl_coefficients(ColorNclPreset preset) {
  switch (preset) {
    case ColorNclPreset::Bt601:
      return Result<std::array<double, 2>>(std::array<double, 2>{.299, .114});
    case ColorNclPreset::Bt709:
      return Result<std::array<double, 2>>(std::array<double, 2>{.2126, .0722});
    case ColorNclPreset::Bt2020:
      return Result<std::array<double, 2>>(std::array<double, 2>{.2627, .0593});
  }
  return Result<std::array<double, 2>>(invalid("unknown NCL preset"));
}
Result<ValueFacet> encode_color_array(const ColorArrayDescriptor& s) {
  return encode(s, false);
}
Result<ColorArrayDescriptor> decode_color_array(const ValueFacet& facet) {
  return decode(facet, false);
}
Result<std::string> color_array_parameter(const ColorArrayDescriptor& s) {
  auto encoded = encode(s, true);
  if (!encoded.ok())
    return Result<std::string>(encoded.status());
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(2 * encoded.value().payload.size());
  for (auto byte : encoded.value().payload) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return Result<std::string>(std::move(result));
}
Result<ColorArrayDescriptor> color_array_from_parameter(
    const std::string& parameter) {
  if (parameter.empty() || parameter.size() > 8192 || parameter.size() % 2)
    return Result<ColorArrayDescriptor>(
        invalid("invalid color parameter size"));
  const auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
  };
  ValueFacet facet{"photospider.color-array", 1, {}};
  facet.payload.reserve(parameter.size() / 2);
  for (std::size_t i = 0; i < parameter.size(); i += 2) {
    const auto a = digit(parameter[i]), b = digit(parameter[i + 1]);
    if (a < 0 || b < 0)
      return Result<ColorArrayDescriptor>(invalid("noncanonical color hex"));
    facet.payload.push_back(static_cast<std::uint8_t>(a * 16 + b));
  }
  return decode(facet, true);
}
Status validate_color_array_descriptor(const ColorArrayDescriptor& s,
                                       const ValueDescriptor& d) {
  auto status = validate(s, false);
  if (!status.ok())
    return status;
  const auto mismatch = [] {
    return Status::failure(ErrorCode::TypeMismatch,
                           "color-array dtype/shape mismatch");
  };
  if ((d.element_type != ElementType::Float32 &&
       d.element_type != ElementType::Float64) ||
      d.shape.size() < 2 || d.shape.size() > 8 || d.shape.back() != channels(s))
    return mismatch();
  std::uint64_t elements = 1;
  for (auto n : d.shape) {
    if (!n || n > (1ULL << 40) / elements)
      return mismatch();
    elements *= n;
  }
  return Status::success();
}
Status validate_color_array_value(const ColorArrayDescriptor& s,
                                  const Value& value, ErrorCode failure,
                                  const std::function<ErrorCode()>& stop) {
  if (!value.valid())
    return invalid("invalid color-array Value");
  auto status = validate_color_array_descriptor(s, value.descriptor());
  if (!status.ok())
    return status;
  const auto& dims = value.region().dimensions();
  if (value.region().empty() || dims.back().offset != 0 ||
      dims.back().extent != channels(s))
    return Status::failure(ErrorCode::TypeMismatch,
                           "color coverage omits complete tuple");
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Status::failure(ErrorCode::OperationFailed,
                           "cannot set color numeric environment");
  std::vector<std::uint64_t> index;
  index.reserve(dims.size());
  for (auto d : dims)
    index.push_back(d.offset);
  std::uint64_t visited = 0;
  std::array<double, 4> color{};
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
    double sample;
    const auto* data = value.bytes().data() + address.value();
    if (value.descriptor().element_type == ElementType::Float32) {
      float narrow;
      std::memcpy(&narrow, data, 4);
      sample = narrow;
    } else {
      std::memcpy(&sample, data, 8);
    }
    bool valid = finite(sample);
    const auto channel = index.back();
    color[channel] = sample;
    auto reason = FailureReason::InvalidDomain;
    if (s.model == ColorModel::Cmyk)
      valid = valid && sample >= 0 && sample <= 1;
    if ((s.model == ColorModel::Cielch || s.model == ColorModel::Oklch) &&
        channel == 1)
      valid = valid && sample >= 0;
    if (s.model == ColorModel::Rgb && channel == 3) {
      valid = valid && sample >= 0 && sample <= 1;
      if (valid && s.association == ColorAssociation::Premultiplied &&
          sample == 0 && (color[0] != 0 || color[1] != 0 || color[2] != 0)) {
        valid = false;
        reason = FailureReason::InvalidAssociation;
      }
    }
    if (!valid)
      return {failure, "sample violates color-array domain", reason};
    std::size_t axis = index.size();
    while (axis) {
      --axis;
      if (++index[axis] < dims[axis].offset + dims[axis].extent)
        break;
      index[axis] = dims[axis].offset;
    }
    if (!axis && index[0] == dims[0].offset)
      break;
  }
  return Status::success();
}
}  // namespace ps
