#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/core/status.hpp"

namespace ps::tools {
struct ToolError : std::runtime_error {
  int exit_code;
  std::string id, status;
  std::uint64_t node = 0, input = 0;
  FailureReason reason = FailureReason::None;
  explicit ToolError(std::string message, int code = 1,
                     std::string diagnostic = "PS-INPUT-INVALID")
      : std::runtime_error(std::move(message)),
        exit_code(code),
        id(std::move(diagnostic)) {}
};
void check(Status status);
template <class T>
T take(Result<T> result) {
  check(result.status());
  return result.take_value();
}
}  // namespace ps::tools
