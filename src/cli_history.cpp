#include "cli_history.hpp"
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace ps {

CliHistory::CliHistory() {
    Load();
}

std::filesystem::path CliHistory::GetHistoryFilePath() const {
    std::filesystem::path home_dir;
#ifdef _WIN32
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path) == S_OK) {
        home_dir = path;
    }
#else
    const char* home = getenv("HOME");
    if (home == nullptr) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (home) {
        home_dir = home;
    }
#endif
    if (home_dir.empty()) {
        return ".photospider_history"; // fallback to current dir
    }
    return home_dir / ".photospider_history";
}


void CliHistory::Load() {
    std::ifstream file(GetHistoryFilePath());
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            history_.push_back(line);
        }
    }
    nav_index_ = history_.size();
}

void CliHistory::Save() const {
    std::ofstream file(GetHistoryFilePath());
    if (!file) {
        std::cerr << "Warning: Could not save command history to " << GetHistoryFilePath() << std::endl;
        return;
    }
    for (const auto& line : history_) {
        file << line << "\n";
    }
}

void CliHistory::Add(const std::string& command) {
    if (command.empty()) return;

    // Don't add if it's the same as the last command
    if (!history_.empty() && history_.back() == command) {
        nav_index_ = history_.size();
        return;
    }

    history_.push_back(command);
    Trim();
    nav_index_ = history_.size();
}

void CliHistory::Trim() {
    if (max_size_ == 0) return; // 0 means infinite
    while (history_.size() > max_size_) {
        history_.erase(history_.begin());
    }
}

std::string CliHistory::GetPrevious(const std::string& current_prefix) {
    if (history_.empty()) return current_prefix;
    
    int start_index = nav_index_ - 1;
    for (int i = start_index; i >= 0; --i) {
        if (history_[i].rfind(current_prefix, 0) == 0) { // starts_with
            nav_index_ = i;
            return history_[i];
        }
    }

    // No match found
    return current_prefix;
}

std::string CliHistory::GetNext(const std::string& current_prefix) {
    if (history_.empty()) return "";

    int start_index = nav_index_ + 1;
    for (size_t i = start_index; i < history_.size(); ++i) {
        if (history_[i].rfind(current_prefix, 0) == 0) { // starts_with
            nav_index_ = i;
            return history_[i];
        }
    }
    
    // If we've reached the end, reset nav and return original prefix
    nav_index_ = history_.size();
    return current_prefix;
}

void CliHistory::ResetNavigation() {
    nav_index_ = history_.size();
}

void CliHistory::SetMaxSize(size_t size) {
    max_size_ = size;
    Trim();
}


} // namespace ps

