#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

int main(int argc, char** argv) {
  try {
    using namespace inpaint_example;  // NOLINT(build/namespaces)
    const std::string key =
        argc > 1 ? argv[1]
                 : "image.local_inpaint_navier_stokes_native_apple_silicon";
    std::vector<float> image(100), mask(25);
    for (unsigned i = 0; i < 25; ++i) {
      image[i * 4] = .25F;
      image[i * 4 + 1] = .5F;
      image[i * 4 + 2] = .75F;
      image[i * 4 + 3] = 1;
    }
    mask[12] = 1;
    const auto input = value(image, {5, 5, 4}, ps::rgba_semantics());
    auto result =
        take(run(key, {input, value(mask, {5, 5}, ps::coverage_semantics())},
                 {{"radius", std::int64_t{3}}}));
    const auto output = result.values.at("result");
    const auto actual = pixels(output);
    for (unsigned i = 0; i < 100; ++i)
      check(std::isfinite(actual[i]) && std::abs(actual[i] - image[i]) < 1e-6,
            "constant oracle");
    check(output.facets()[0].payload == input.facets()[0].payload,
          "preserved image semantics");
    std::cout << key << " named image: 5x5 RGBA; center=" << actual[48] << ','
              << actual[49] << ',' << actual[50] << ',' << actual[51]
              << " PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
