#include "plugin/utf8_validation.hpp"

#include <cstddef>
#include <cstdint>

namespace ps::plugin_internal {

/**
 * @brief Implements strict bounded UTF-8 registry-key validation.
 * @copydetails valid_utf8_key
 */
bool valid_utf8_key(std::string_view value) noexcept {
  if (value.empty() || value.size() > 1024U) {
    return false;
  }
  std::size_t index = 0U;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      if (first < 0x20U || first == 0x7fU) {
        return false;
      }
      ++index;
      continue;
    }

    std::size_t width = 0U;
    std::uint32_t code_point = 0U;
    std::uint32_t minimum = 0U;
    if ((first & 0xe0U) == 0xc0U) {
      width = 2U;
      code_point = first & 0x1fU;
      minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      width = 3U;
      code_point = first & 0x0fU;
      minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      width = 4U;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (width > value.size() - index) {
      return false;
    }
    for (std::size_t offset = 1U; offset < width; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if (code_point < minimum || code_point > 0x10ffffU ||
        (code_point >= 0xd800U && code_point <= 0xdfffU)) {
      return false;
    }
    index += width;
  }
  return true;
}

}  // namespace ps::plugin_internal
