#pragma once
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "common/error.hpp"

namespace ps::tools {
struct Diagnostics final {
  bool json = false, color = false, verbose = false, timings = false;
  std::string stage = "driver", source;
  std::optional<std::size_t> revision;
  std::vector<std::string> warnings;
  void event(const std::string& severity, const std::string& id,
             const std::string& message, const std::string& code = {},
             std::uint64_t node = 0, std::uint64_t input = 0) const;
  void warning(const std::string& group, const std::string& message) const;
  void timing(const std::string& name,
              std::chrono::steady_clock::time_point start) const;
};

int report_exception(const Diagnostics& diagnostics, std::exception_ptr error);
}  // namespace ps::tools
