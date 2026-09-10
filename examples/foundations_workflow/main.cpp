#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

int main(int argc, char** argv) {
  try {
    std::string scenario = "all";
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "--scenario "
                   "all|cast-range|channels|alpha-color|expression-lut|"
                   "generator-gain|components\n";
      return 0;
    }
    if (argc != 1) {
      foundations::require(argc == 3 && std::string(argv[1]) == "--scenario",
                           "use --scenario NAME or --help");
      scenario = argv[2];
    }
    const std::vector<std::pair<std::string, void (*)()>> scenes = {
        {"cast-range", foundations::numeric},
        {"channels", foundations::channels},
        {"alpha-color", foundations::alpha_color},
        {"expression-lut", foundations::expressions},
        {"generator-gain", foundations::generator_gain},
        {"components", foundations::components}};
    unsigned ran = 0;
    for (const auto& scene : scenes)
      if (scenario == "all" || scenario == scene.first) {
        scene.second();
        ++ran;
      }
    foundations::require(ran > 0, "unknown scenario: " + scenario);
    std::cout << "Foundations scenarios=" << ran
              << " oracle=passed backend=cpu\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Foundations failed: " << error.what() << '\n';
    return 1;
  }
}
