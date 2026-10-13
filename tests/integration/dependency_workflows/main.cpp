#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/dependency_workflow_fixture.hpp"

void reductions_workflow();
void scan_workflow();
void block_cache_workflow();

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--scenario") {
      const std::pair<const char*, void (*)()> scenarios[] = {
          {"reductions", reductions_workflow},
          {"scan", scan_workflow},
          {"blocks", block_cache_workflow}};
      for (const auto& scenario : scenarios)
        if (std::string(argv[2]) == scenario.first) {
          scenario.second();
          return 0;
        }
      throw std::runtime_error("unknown dependency scenario");
    }
    if (argc != 1)
      throw std::runtime_error(
          "usage: test_dependency_workflows [--scenario "
          "reductions|scan|blocks]");
    reductions_workflow();
    scan_workflow();
    block_cache_workflow();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
