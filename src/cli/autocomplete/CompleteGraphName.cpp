#include <string>
#include <vector>

#include "cli/cli_autocompleter.hpp"
#include "photospider/host/host.hpp"

namespace ps {

void CliAutocompleter::CompleteGraphName(
    const std::string& prefix, std::vector<std::string>& options) const {
  auto names = svc_.list_graphs();
  if (!names.status.ok) {
    return;
  }
  for (const auto& n : names.value) {
    if (n.value.rfind(prefix, 0) == 0)
      options.push_back(n.value);
  }
}

}  // namespace ps
