#include <sstream>
#include <string>
#include <vector>

#include "graph_cli/cli_autocompleter.hpp"

namespace ps {

/** @copydoc CliAutocompleter::Tokenize */
std::vector<std::string> CliAutocompleter::Tokenize(
    const std::string& line) const {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

}  // namespace ps
