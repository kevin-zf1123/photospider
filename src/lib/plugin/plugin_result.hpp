#pragma once

#include <string>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

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
 * lists operation keys touched by successful registration, including keys that
 * replaced an existing implementation.
 *
 * @note The field keeps its legacy name for callers, but its values now
 * describe registered or replaced keys so unloadable plugin handles can be
 * tracked safely.
 */
struct PluginLoadResult {
  int attempted = 0;
  int loaded = 0;
  std::vector<PluginLoadError> errors;
  std::vector<std::string> new_op_keys;
};

}  // namespace ps
