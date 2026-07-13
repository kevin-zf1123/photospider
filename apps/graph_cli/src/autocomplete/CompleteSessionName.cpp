#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "graph_cli/cli_autocompleter.hpp"

namespace ps {

/** @copydoc CliAutocompleter::CompleteSessionName */
void CliAutocompleter::CompleteSessionName(
    const std::string& prefix, std::vector<std::string>& options) const {
  namespace fs = std::filesystem;
  try {
    fs::path sessions_dir("sessions");
    if (!fs::exists(sessions_dir) || !fs::is_directory(sessions_dir))
      return;
    for (const auto& entry : fs::directory_iterator(sessions_dir)) {
      if (!fs::is_directory(entry.status()))
        continue;
      auto name = entry.path().filename().string();
      if (name.rfind(prefix, 0) == 0)
        options.push_back(name);
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    // ignore filesystem errors
  }
}

}  // namespace ps
