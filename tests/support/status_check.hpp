#pragma once

#include <iostream>

#include "photospider/core/status.hpp"

namespace ps::test {
/**
 * @brief Reports one failed test condition with source location.
 * @param condition Evaluated condition.
 * @param expression Source expression text.
 * @param file Source filename.
 * @param line Source line.
 * @return True when the condition passed.
 * @throws Nothing.
 */
inline bool check(bool condition, const char* expression, const char* file,
                  int line) noexcept {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
  }
  return condition;
}

/** @brief Prints the full failure before a test attempts to unwrap a result. */
inline bool require_ok(const Status& status, const char* expression,
                       const char* file, int line) {
  if (status.ok())
    return true;
  std::cerr << file << ':' << line << ": " << expression
            << " failed: code=" << static_cast<int>(status.code)
            << " reason=" << static_cast<int>(status.reason)
            << " origin=" << static_cast<int>(status.detail.origin)
            << " scope=" << static_cast<int>(status.detail.scope)
            << " node=" << status.detail.node_id
            << " input=" << status.detail.input_id;
  if (status.detail.atom) {
    const auto& atom = *status.detail.atom;
    std::cerr << " atom=" << atom.output_index << '[';
    for (std::uint32_t axis = 0; axis < atom.rank; ++axis)
      std::cerr << (axis ? "," : "") << atom.coordinate[axis];
    std::cerr << ']';
  }
  std::cerr << " message=" << status.message << '\n';
  return false;
}
template <class T>
inline bool require_ok(const Result<T>& result, const char* expression,
                       const char* file, int line) {
  return require_ok(result.status(), expression, file, line);
}

}  // namespace ps::test

#define PS_CHECK(expression)                                           \
  do {                                                                 \
    if (!::ps::test::check(static_cast<bool>(expression), #expression, \
                           __FILE__, __LINE__)) {                      \
      return 1;                                                        \
    }                                                                  \
  } while (false)

#define PS_REQUIRE_OK(expression)                                    \
  do {                                                               \
    if (!::ps::test::require_ok((expression), #expression, __FILE__, \
                                __LINE__))                           \
      return 1;                                                      \
  } while (false)
