#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/status.hpp"

namespace ps::plugin_internal::numeric_ops {
inline Status array_parameter_error(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::InvalidDomain,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}
// Canonical ASCII decimal lists are bounded before adding another axis.
inline Result<std::vector<std::uint64_t>> parse_array_list(
    const std::string& text, bool shape) {
  using Answer = Result<std::vector<std::uint64_t>>;
  std::vector<std::uint64_t> result;
  std::uint64_t product = 1;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    const auto limit = end == std::string::npos ? text.size() : end;
    if (result.size() == 8 || limit == begin ||
        (text[begin] == '0' && limit != begin + 1))
      return Answer(array_parameter_error("noncanonical array list"));
    std::uint64_t value = 0;
    for (auto i = begin; i < limit; ++i) {
      if (text[i] < '0' || text[i] > '9' ||
          value > (UINT64_MAX - (text[i] - '0')) / 10)
        return Answer(array_parameter_error("invalid array list integer"));
      value = value * 10 + (text[i] - '0');
    }
    if (shape) {
      if (!value || value > (UINT64_C(1) << 40) / product)
        return Answer(array_parameter_error("shape exceeds 2^40 elements"));
      product *= value;
    }
    result.push_back(value);
    if (end == std::string::npos)
      break;
    begin = end + 1;
    if (begin == text.size())
      return Answer(array_parameter_error("trailing array list comma"));
  }
  if (result.empty())
    return Answer(array_parameter_error("empty array list"));
  return Answer(std::move(result));
}
}  // namespace ps::plugin_internal::numeric_ops
