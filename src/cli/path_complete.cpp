#include "cli/path_complete.hpp"
#include <filesystem>

namespace ps {

namespace fs = std::filesystem;

std::vector<std::string> PathCompleteOptions(const std::string& prefix) {
    std::vector<std::string> options;

    std::string list_dir = ".";          // directory to iterate
    std::string completion_prefix;         // text to keep in front of filename
    std::string basename_prefix = prefix;  // part to match within the directory

    auto last_slash = prefix.find_last_of('/');
    if (last_slash != std::string::npos) {
        completion_prefix = prefix.substr(0, last_slash + 1);
        basename_prefix = prefix.substr(last_slash + 1);
        list_dir = completion_prefix;
    } else {
        completion_prefix.clear();
        basename_prefix = prefix;
        list_dir = ".";
    }

    try {
        fs::path dir(list_dir);
        if (!fs::exists(dir) || !fs::is_directory(dir)) return options;

        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string filename = entry.path().filename().string();
            if (filename.rfind(basename_prefix, 0) == 0) {
                std::string completion = completion_prefix + filename;
                if (fs::is_directory(entry.path())) {
                    completion += "/";
                }
                options.push_back(std::move(completion));
            }
        }
    } catch (const fs::filesystem_error&) {
        // ignore
    }

    return options;
}

std::string LongestCommonPrefix(const std::vector<std::string>& options) {
    if (options.empty()) return "";
    if (options.size() == 1) return options[0];
    std::string prefix = options[0];
    for (size_t i = 1; i < options.size(); ++i) {
        while (options[i].rfind(prefix, 0) != 0) {
            if (prefix.empty()) return "";
            prefix.pop_back();
        }
    }
    return prefix;
}

} // namespace ps
