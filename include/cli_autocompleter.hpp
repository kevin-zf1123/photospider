#pragma once
#include <string>
#include <vector>
#include <map>
#include "node_graph.hpp"

namespace ps {

struct CompletionResult {
    std::vector<std::string> options;
    std::string new_line;
    int new_cursor_pos;
};

class CliAutocompleter {
public:
    CliAutocompleter(const NodeGraph& graph, const std::map<std::string, std::string>& op_sources);

    CompletionResult Complete(const std::string& line, int cursor_pos);

private:
    std::vector<std::string> Tokenize(const std::string& line) const;
    std::string FindLongestCommonPrefix(const std::vector<std::string>& options) const;

    // Completion providers
    void CompleteCommand(const std::string& prefix, std::vector<std::string>& options) const;
    void CompletePath(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteNodeId(const std::string& prefix, std::vector<std::string>& options) const;
    void CompletePrintArgs(const std::string& prefix, std::vector<std::string>& options, bool only_mode_args) const;
    void CompleteTraversalArgs(const std::string& prefix, std::vector<std::string>& options) const;
    void CompleteComputeArgs(const std::string& prefix, std::vector<std::string>& options) const;

    const NodeGraph& graph_;
    std::vector<std::string> commands_;
};

} // namespace ps
