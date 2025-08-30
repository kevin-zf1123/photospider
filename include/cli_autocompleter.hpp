#pragma once
#include <string>
#include <vector>
#include <map>

namespace ps { class InteractionService; }

namespace ps {

struct CompletionResult {
    std::vector<std::string> options;
    std::string new_line;
    int new_cursor_pos;
};

class CliAutocompleter {
public:
    // Build a completer bound to the backend interaction layer.
    // op_sources is optional and may be empty; it's only used for `ops` listing, not for completion.
    explicit CliAutocompleter(ps::InteractionService& svc);

    // Set or update the current graph name so dynamic completions (node ids) can be provided.
    void SetCurrentGraph(const std::string& graph_name) { current_graph_ = graph_name; }

    CompletionResult Complete(const std::string& line, int cursor_pos);

private:
    std::vector<std::string> Tokenize(const std::string& line) const;
    std::string FindLongestCommonPrefix(const std::vector<std::string>& options) const;

    // Completion providers
    void CompleteCommand(const std::string& prefix, std::vector<std::string>& options) const;
    void CompletePath(const std::string& prefix, std::vector<std::string>& options) const;
    // Complete only YAML files (and allow directories to continue traversal).
    void CompleteYamlPath(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteNodeId(const std::string& prefix, std::vector<std::string>& options) const;
    void CompletePrintArgs(const std::string& prefix, std::vector<std::string>& options, bool only_mode_args) const;
    void CompleteTraversalArgs(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteComputeArgs(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteGraphName(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteOpsMode(const std::string& prefix, std::vector<std::string>& options) const;
    // Complete session/graph names by scanning the `sessions` directory for subdirectories.
    void CompleteSessionName(const std::string& prefix, std::vector<std::string>& options) const;

    ps::InteractionService& svc_;
    std::string current_graph_;
    std::vector<std::string> commands_;
};

} // namespace ps
