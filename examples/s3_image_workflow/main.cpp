#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coordinator.hpp"  // NOLINT(build/include_subdir)

namespace {
void preview(const std::shared_ptr<ps::OperationRegistry>& registry) {
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
        const float color[4] = {event == 9 ? 0.F : .5F, event == 9 ? .5F : 0.F,
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
            << " rejected=" << app.rejected << " quality=full oracle=passed\n";
}
std::uint64_t calls(const ps::ExecutionResult& result, std::uint64_t id = 0) {
  std::uint64_t total = 0;
  for (const auto& timing : result.diagnostics.operation_timings)
    if (id == 0 || timing.output.node_id == id)
      total += timing.invocation_count;
  return total;
}
void cache(const std::shared_ptr<ps::OperationRegistry>& registry) {
  s3::Scene scene(registry);
  ps::ExecutionContext execution(registry,
                                 {2, false, 16, 1024 * 1024, 256 * 1024});
  auto first = s3::take(execution.execute(scene.freeze(execution, 2)));
  s3::require(calls(first, 10) == 20, "cold blur tile count");
  auto exposure = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(exposure, 10) == 0, "exposure must reuse blur");
  scene.stamp(execution, {4, 5, 2, 1, 0, 0, .5F});
  auto changed = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(changed, 10) > 0 && calls(changed, 10) < 20,
              "local stamp must retain unrelated blur regions");
  s3::Frame output(s3::Scene::height, s3::Scene::width);
  output.blit(changed.values.at("result"));
  output.check(scene.oracle(3));
  scene.edit_unrelated_branch();
  auto unrelated = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(unrelated) == 0,
              "unrelated graph edit must preserve results");
  execution.clear_result_cache();
  auto rebuilt = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(rebuilt.values.at("result").copy_bytes() ==
                  unrelated.values.at("result").copy_bytes(),
              "cache deletion correctness");
  std::cout << "S3Cache.LocalInvalidation blur_after_gain=0 patch_blur_tiles="
            << calls(changed, 10) << " unrelated_callbacks=0 oracle=passed\n";
}
void disk(const std::shared_ptr<ps::OperationRegistry>& registry,
          const std::string& mode, const std::string& directory) {
  s3::require(!directory.empty(),
              "disk scenarios require --cache-dir DIRECTORY");
  s3::require(!registry->persistent_cache_identity().empty(),
              "disk profile requires maintained built-in registry");
  if (mode == "disk-corrupt") {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() != ".pscache")
        continue;
      std::fstream file(entry.path(),
                        std::ios::in | std::ios::out | std::ios::binary);
      file.put('!');
    }
  }
  ps::ExecutionContextConfig config{2, false, 16, 1024 * 1024, 256 * 1024};
  config.disk_cache = ps::DiskCacheConfig{directory, 1024 * 1024, 4096, 512};
  ps::ExecutionContext execution(registry, config);
  s3::Scene scene(registry);
  if (mode == "disk-clear")
    execution.clear_disk_cache();
  auto result = s3::take(execution.execute(scene.freeze(execution, 2)));
  s3::Frame frame(s3::Scene::height, s3::Scene::width);
  frame.blit(result.values.at("result"));
  frame.check(scene.oracle(2));
  execution.flush_disk_cache();
  const auto stats = execution.disk_cache_statistics();
  if (mode == "disk-read")
    s3::require(stats.hits > 0,
                "no disk hits; run disk-write first with this build");
  if (mode == "disk-corrupt")
    s3::require(stats.invalid_entries > 0,
                "no corrupted entries; run disk-write first");
  std::cout << "S3Disk.DiscardAndRebuild mode=" << mode
            << " hits=" << stats.hits << " invalid=" << stats.invalid_entries
            << " oracle=passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string scenario = "preview", module, directory;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "--scenario "
                     "cache|preview|disk-write|disk-read|disk-corrupt|disk-"
                     "clear [--cache-dir DIRECTORY] [--module PATH]\n";
        return 0;
      }
      s3::require(i + 1 < argc, "missing option value");
      if (option == "--scenario")
        scenario = argv[++i];
      else if (option == "--module")
        module = argv[++i];
      else if (option == "--cache-dir")
        directory = argv[++i];
      else
        throw std::runtime_error("unknown option: " + option);
    }
    auto registry = module.empty() ? ps::make_default_operation_registry()
                                   : std::make_shared<ps::OperationRegistry>();
    if (!module.empty()) {
      s3::require(registry->load_plugin(module).ok(), "module load failed");
      s3::require(registry->freeze().ok(), "registry freeze failed");
    }
    if (scenario == "preview")
      preview(registry);
    else if (scenario == "cache")
      cache(registry);
    else if (scenario == "disk-write" || scenario == "disk-read" ||
             scenario == "disk-corrupt" || scenario == "disk-clear")
      disk(registry, scenario, directory);
    else
      throw std::runtime_error("unknown scenario: " + scenario);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
