#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coordinator.hpp"  // NOLINT(build/include_subdir)

int main(int argc, char** argv) {
  try {
    auto registry = argc == 2 ? std::make_shared<ps::OperationRegistry>()
                              : ps::make_default_operation_registry();
    if (argc == 2) {
      s3::require(registry->load_plugin(argv[1]).ok(), "module load failed");
      s3::require(registry->freeze().ok(), "registry freeze failed");
    }
    s3::Scene scene(registry);
    ps::ExecutionContext execution(registry,
                                   {2, false, 16, 1024 * 1024, 256 * 1024});
    const auto export_oracle = scene.oracle(2);
    s3::Coordinator app(scene, execution);
    s3::require(app.begin_export(), "export admission");
    for (int i = 0; i < 8; ++i)
      s3::require(app.brush({static_cast<float>(i + 1), 3, 2, 1, 0, .25F, .5F}),
                  "stamp admission");
    s3::require(!app.brush({9, 3, 2, 0, 1, 0, .5F}),
                "stamp queue must apply backpressure");
    app.tick();
    s3::require(app.brush({9, 3, 2, 0, 1, 0, .5F}), "retry after progress");
    app.slider(3);
    app.slider(4);
    for (unsigned ticks = 0; !app.idle() && ticks < 200; ++ticks)
      app.tick();
    s3::require(app.idle() && app.applied_stamps == 9 && app.gain == 4,
                "bounded event replay convergence");
    std::vector<float> expected_input(s3::Scene::height * s3::Scene::width * 4);
    for (std::size_t i = 0; i < expected_input.size(); ++i)
      expected_input[i] = i % 4 == 3 ? .5F : .125F;
    for (int event = 1; event <= 9; ++event)
      for (std::uint64_t y = 0; y < s3::Scene::height; ++y)
        for (std::uint64_t x = 0; x < s3::Scene::width; ++x) {
          if (std::hypot(static_cast<double>(x) + .5 - event,
                         static_cast<double>(y) + .5 - 3) > 2)
            continue;
          const float color[4] = {event == 9 ? 0.F : .5F,
                                  event == 9 ? .5F : 0.F,
                                  event == 9 ? 0.F : .125F, .5F};
          for (std::size_t c = 0; c < 4; ++c) {
            auto& sample = expected_input[(y * s3::Scene::width + x) * 4 + c];
            sample = color[c] + sample * .5F;
          }
        }
    std::vector<float> actual_input(expected_input.size());
    s3::require(
        scene.foreground
            .read(ps::Region::whole({s3::Scene::height, s3::Scene::width, 4}),
                  reinterpret_cast<std::uint8_t*>(actual_input.data()),
                  actual_input.size() * 4)
            .ok(),
        "stroke oracle read");
    s3::require(actual_input == expected_input, "exact ordered stamp oracle");
    app.exported.check(export_oracle);
    app.displayed.check(scene.oracle(4));
    s3::require(app.displayed_quality == 1 && app.export_tiles == 20,
                "full quality and finite export progress");
    s3::require(!app.publish(app.version - 1, 1, "viewer", app.exported),
                "stale publication rejected");
    s3::require(!app.publish(app.version, 0, "viewer", app.displayed),
                "quality downgrade rejected");
    s3::require(!app.publish(app.version, 1, "other", app.displayed),
                "foreign target rejected");
    std::cout << "S3Preview.LatestAndExport stamps=" << app.applied_stamps
              << " export_tiles=" << app.export_tiles
              << " preview_tiles=" << app.preview_tiles
              << " rejected=" << app.rejected
              << " quality=full oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
