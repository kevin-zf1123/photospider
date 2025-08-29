#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace ps {

class CliHistory {
public:
    CliHistory();

    void Load();
    void Save() const;
    void Add(const std::string& command);

    // Get previous/next command that matches the given prefix
    std::string GetPrevious(const std::string& current_prefix);
    std::string GetNext(const std::string& current_prefix);
    
    // Resets the navigation index (e.g., when a new command is typed)
    void ResetNavigation();

    void SetMaxSize(size_t size);

    // Expose the resolved history file path for UI/diagnostics.
    const std::filesystem::path& Path() const { return history_file_path_; }

private:
    std::filesystem::path GetHistoryFilePath() const;
    void Trim();

    std::vector<std::string> history_;
    int nav_index_ = -1;
    size_t max_size_ = 1000;
    std::filesystem::path history_file_path_;
};

} // namespace ps
