#pragma once

#include <string>
#include <vector>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief One operation or scheduler plugin load failure.
 *
 * @note `path` is the attempted plugin path whenever the loader can determine
 * it; `message` contains platform loader, missing-symbol, or registration
 * error text suitable for frontend reporting.
 */
struct PluginLoadError {
  std::string path;
  GraphErrc code = GraphErrc::Unknown;
  std::string message;
};

/**
 * @brief Structured result from an operation plugin scan.
 *
 * `attempted` counts candidate shared libraries that were actually opened or
 * considered for opening. `loaded` counts libraries whose registration entry
 * point completed successfully and whose handle was retained. `new_op_keys`
 * lists operation keys that appeared in `OpRegistry` after successful
 * registration.
 *
 * @note A plugin can be counted as loaded with no `new_op_keys` if its entry
 * point succeeds but only re-registers existing operations.
 */
struct PluginLoadResult {
  int attempted = 0;
  int loaded = 0;
  std::vector<PluginLoadError> errors;
  std::vector<std::string> new_op_keys;
};

}  // namespace ps
