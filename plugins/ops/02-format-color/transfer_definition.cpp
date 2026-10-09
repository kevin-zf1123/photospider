#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "photospider/ops/format/transfer.hpp"

namespace ps {
namespace {
Status invalid() {
  return {ErrorCode::InvalidArgument,
          "invalid FMT-09 transfer definition",
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::string hex(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  std::string result(16, '0');
  for (unsigned i = 0; i < 16; ++i)
    result[15 - i] = "0123456789abcdef"[(bits >> (4 * i)) & 15];
  return result;
}
bool unhex(const std::string& text, double* value) {
  if (text.size() != 16)
    return false;
  std::uint64_t bits = 0;
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
    bits = (bits << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
  }
  std::memcpy(value, &bits, 8);
  return std::isfinite(*value);
}
const char* name(TransferCurve curve) {
  switch (curve) {
    case TransferCurve::Linear:
      return "linear";
    case TransferCurve::PowerGamma:
      return "power_gamma";
    case TransferCurve::Srgb:
      return "srgb";
    case TransferCurve::Bt709:
      return "bt709";
    case TransferCurve::Bt2020:
      return "bt2020";
    case TransferCurve::Bt1886:
      return "bt1886";
    case TransferCurve::Pq:
      return "pq";
    case TransferCurve::HlgOetf:
      return "hlg_oetf";
    case TransferCurve::Acescc:
      return "acescc";
    case TransferCurve::Acescct:
      return "acescct";
  }
  return nullptr;
}
}  // namespace
Result<std::string> encode_transfer_definition(const TransferDefinition& d) {
  using R = Result<std::string>;
  const auto* id = name(d.curve);
  if (!id || (d.curve != TransferCurve::PowerGamma && d.gamma) ||
      (d.curve != TransferCurve::Bt2020 && d.coefficient_variant) ||
      (d.curve != TransferCurve::Bt1886 &&
       (d.black_luminance || d.white_luminance)))
    return R(invalid());
  if (d.curve == TransferCurve::PowerGamma) {
    if (!d.gamma || !std::isfinite(*d.gamma) || *d.gamma <= 0)
      return R(invalid());
    return R("fmt09-v1:power_gamma:" + hex(*d.gamma));
  }
  if (d.curve == TransferCurve::Bt2020) {
    const char* variant = nullptr;
    switch (d.coefficient_variant.value_or(Bt2020Coefficients::Smooth)) {
      case Bt2020Coefficients::Smooth:
        variant = "smooth";
        break;
      case Bt2020Coefficients::Rounded10Bit:
        variant = "rounded_10bit";
        break;
      case Bt2020Coefficients::Rounded12Bit:
        variant = "rounded_12bit";
        break;
    }
    if (!variant)
      return R(invalid());
    return R(std::string("fmt09-v1:bt2020:") + variant);
  }
  if (d.curve == TransferCurve::Bt1886) {
    if (!d.black_luminance || !d.white_luminance ||
        !std::isfinite(*d.black_luminance) ||
        !std::isfinite(*d.white_luminance) || *d.black_luminance < 0 ||
        *d.black_luminance >= *d.white_luminance)
      return R(invalid());
    return R("fmt09-v1:bt1886:" + hex(*d.black_luminance) + ":" +
             hex(*d.white_luminance));
  }
  return R(std::string(id));
}
Result<TransferDefinition> decode_transfer_definition(const std::string& text) {
  using R = Result<TransferDefinition>;
  if (text.size() > 128)
    return R(invalid());
  TransferDefinition d;
  for (auto c :
       {TransferCurve::Linear, TransferCurve::Srgb, TransferCurve::Bt709,
        TransferCurve::Pq, TransferCurve::HlgOetf, TransferCurve::Acescc,
        TransferCurve::Acescct}) {
    if (text == name(c)) {
      d.curve = c;
      return R(d);
    }
  }
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    auto end = text.find(':', start);
    fields.push_back(text.substr(start, end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  if (fields.size() < 3 || fields[0] != "fmt09-v1")
    return R(invalid());
  double a = 0, b = 0;
  if (fields[1] == "power_gamma" && fields.size() == 3 &&
      unhex(fields[2], &a)) {
    d.curve = TransferCurve::PowerGamma;
    d.gamma = a;
  } else if (fields[1] == "bt1886" && fields.size() == 4 &&
             unhex(fields[2], &a) && unhex(fields[3], &b)) {
    d.curve = TransferCurve::Bt1886;
    d.black_luminance = a;
    d.white_luminance = b;
  } else if (fields[1] == "bt2020" && fields.size() == 3) {
    d.curve = TransferCurve::Bt2020;
    if (fields[2] == "smooth")
      d.coefficient_variant = Bt2020Coefficients::Smooth;
    else if (fields[2] == "rounded_10bit")
      d.coefficient_variant = Bt2020Coefficients::Rounded10Bit;
    else if (fields[2] == "rounded_12bit")
      d.coefficient_variant = Bt2020Coefficients::Rounded12Bit;
    else
      return R(invalid());
  } else {
    return R(invalid());
  }
  auto encoded = encode_transfer_definition(d);
  if (!encoded.ok() || encoded.value() != text)
    return R(invalid());
  return R(d);
}
}  // namespace ps
