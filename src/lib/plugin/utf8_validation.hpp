#pragma once

#include <string_view>

namespace ps::plugin_internal {

/**
 * @brief Validates one bounded canonical registry key as strict UTF-8.
 * @param value Exact candidate bytes, which may contain embedded nulls.
 * @return True only for 1..1024 bytes of well-formed UTF-8 without ASCII
 * control bytes.
 * @throws Nothing.
 * @note Validation rejects stray/invalid continuations, truncation, overlong
 * encodings, UTF-16 surrogate code points, and values above U+10FFFF. Unicode
 * normalization is intentionally outside registry identity.
 */
[[nodiscard]] bool valid_utf8_key(std::string_view value) noexcept;

}  // namespace ps::plugin_internal
