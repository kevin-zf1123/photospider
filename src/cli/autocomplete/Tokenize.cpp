#include "cli/cli_autocompleter.hpp"
#include <sstream>

namespace ps {

std::vector<std::string> CliAutocompleter::Tokenize(const std::string& line) const {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace ps