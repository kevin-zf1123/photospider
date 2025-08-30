// moved to include/cli/path_complete.hpp
#pragma once

#include <string>
#include <vector>

namespace ps {

// Return a list of filesystem path completion options for the given prefix.
// - If prefix contains a '/', iterate in that directory and keep the typed
//   directory prefix in the suggestions.
// - If no '/', iterate in current directory but do NOT prepend "./".
std::vector<std::string> PathCompleteOptions(const std::string& prefix);

// Utility: longest common prefix of a non-empty vector of strings.
std::string LongestCommonPrefix(const std::vector<std::string>& options);

} // namespace ps
