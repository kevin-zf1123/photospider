// Kernel plugin loader result and error reporting structures
#pragma once

#include <string>
#include <vector>

#include "ps_types.hpp"

namespace ps {

struct PluginLoadError {
  std::string path;  // attempted plugin path
  GraphErrc code = GraphErrc::Unknown;
  std::string message;  // optional descriptive message
};

struct PluginLoadResult {
  int attempted = 0;
  int loaded = 0;
  std::vector<PluginLoadError> errors;
  std::vector<std::string> new_op_keys;  // fully qualified keys registered
};

}  // namespace ps
