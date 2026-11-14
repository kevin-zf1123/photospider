#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/error.hpp"

namespace ps::tools {
struct CommonOptions {
  std::string input, output;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  std::vector<std::string> libraries, warnings;
  bool has(const std::string& key) const {
    return flags.count(key) || values.count(key);
  }
  std::string get(const std::string& key,
                  const std::string& fallback = {}) const {
    auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
  }
};
struct ParsedOptions final {
  CommonOptions common;
  std::vector<std::pair<std::string, std::string>> repeated;
};
/** Tokenization and common options only; each tool supplies its own option set.
 * Sets diagnostic mode before reporting invalid arguments, respecting the
 * same option/value boundaries as normal parsing. Warning switches are
 * collected only when the caller explicitly enables them.
 */
ParsedOptions parse_arguments(int argc, char** argv, bool* json_diagnostics,
                              std::set<std::string> flags,
                              std::set<std::string> values,
                              const std::set<std::string>& repeatable = {},
                              bool allow_warnings = false);
std::uint64_t unsigned_number(const std::string& text,
                              const std::string& label);
std::uint32_t positive_count(const CommonOptions& options,
                             const std::string& name, std::uint32_t fallback);
std::vector<std::string> output_names(const std::string& text);
void require_input(const CommonOptions& options, bool information);
}  // namespace ps::tools
