#include "cli/cli_autocompleter.hpp"
#include <filesystem>

namespace ps {

void CliAutocompleter::CompleteSessionName(const std::string& prefix, std::vector<std::string>& options) const {
    namespace fs = std::filesystem;
    try {
        fs::path sessions_dir("sessions");
        if (!fs::exists(sessions_dir) || !fs::is_directory(sessions_dir)) return;
        for (const auto& entry : fs::directory_iterator(sessions_dir)) {
            if (!fs::is_directory(entry.status())) continue;
            auto name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) options.push_back(name);
        }
    } catch (...) {
        // ignore filesystem errors
    }
}

} // namespace ps