#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "s3_image_workflow/coordinator.hpp"

namespace s4 {
inline std::uint64_t calls(const ps::ExecutionResult& result,
                           std::uint64_t node = 0) {
  std::uint64_t total = 0;
  for (const auto& timing : result.diagnostics.operation_timings)
    if (!node || timing.node_id == node)
      total += timing.invocation_count;
  return total;
}
inline ps::ExecutionContextConfig config(ps::ExecutionMode mode,
                                         bool cache = true) {
  ps::ExecutionContextConfig result{2, mode == ps::ExecutionMode::MetalFp32, 16,
                                    1024 * 1024, cache ? 256U * 1024U : 0U};
  return result;
}
/** @brief Exercises bounded native result/input retention and S3 edit
 * semantics. */
inline bool cache_edits(const std::shared_ptr<ps::OperationRegistry>& registry,
                        ps::ExecutionMode mode) {
  s3::Scene scene(registry, mode);
  ps::ExecutionContext execution(registry, config(mode));
  auto first = s3::take(execution.execute(scene.freeze(execution, 2)));
  if (mode == ps::ExecutionMode::MetalFp32 && !execution.gpu_enabled()) {
    s3::Frame image(s3::Scene::height, s3::Scene::width);
    image.blit(first.values.at("result"));
    image.check(scene.oracle(2));
    s3::require(first.diagnostics.native_dispatch_count == 0,
                "unavailable device dispatched");
    std::cout
        << "S4Gpu.CacheEdits native=unavailable CPU_fallback_oracle=passed\n";
    return false;
  }
  s3::require(calls(first, 10) == 20, "cold blur tile count");
  auto warm = s3::take(execution.execute(scene.freeze(execution, 2)));
  s3::require(calls(warm) == 0 && warm.diagnostics.native_dispatch_count == 0 &&
                  warm.diagnostics.transfer_bytes == 0,
              "warm workflow performed work");
  auto gain = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(gain, 10) == 0, "gain edit recomputed blur");
  if (mode == ps::ExecutionMode::MetalFp32 && execution.gpu_enabled()) {
    s3::require(first.diagnostics.native_dispatch_count > 0,
                "cold chain performed no Metal work");
    s3::require(gain.diagnostics.native_upload_hits > 0,
                "retained source buffers not reused");
    s3::require(execution.cache_statistics().native_retained_bytes > 0,
                "no native storage retained");
  }
  scene.stamp(execution, {4, 5, 2, 1, 0, 0, .5F});
  auto changed = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(changed, 10) > 0 && calls(changed, 10) < 20,
              "local stamp invalidation is not regional");
  s3::Frame image(s3::Scene::height, s3::Scene::width);
  image.blit(changed.values.at("result"));
  image.check(scene.oracle(3));
  scene.edit_unrelated_branch();
  auto unrelated = s3::take(execution.execute(scene.freeze(execution, 3)));
  s3::require(calls(unrelated) == 0, "unrelated branch invalidated results");
  if (mode == ps::ExecutionMode::MetalFp32 && execution.gpu_enabled()) {
    auto tiny = s3::take(execution.execute(scene.freeze(execution, 1e-10F)));
    auto repeated =
        s3::take(execution.execute(scene.freeze(execution, 1e-10F)));
    s3::require(!tiny.diagnostics.fallback_reasons.empty() &&
                    calls(repeated, 20) > 0 && calls(repeated, 30) > 0 &&
                    calls(repeated, 40) > 0,
                "fallback ancestry entered Metal result cache");
    s3::Scene exact_scene(registry);
    auto exact = s3::take(execution.execute(exact_scene.freeze(execution, 2)));
    s3::require(
        calls(exact) > 0 && exact.diagnostics.native_dispatch_count == 0,
        "CPU exact reused Metal results");
  }
  const auto stats = execution.cache_statistics();
  s3::require(stats.retained_bytes <= 256 * 1024, "cache sublimit exceeded");
  execution.clear_result_cache();
  s3::require(execution.cache_statistics().retained_bytes == 0,
              "cache clear retained entries");
  auto rebuilt = s3::take(execution.execute(scene.freeze(execution, 3)));
  image.blit(rebuilt.values.at("result"));
  image.check(scene.oracle(3));
  std::cout << "S4Gpu.CacheEdits warm_dispatches=0 blur_after_gain=0 "
               "patch_blur_tiles="
            << calls(changed, 10)
            << " upload_hits=" << gain.diagnostics.native_upload_hits
            << " native_retained_bytes=" << stats.native_retained_bytes
            << " oracle=passed\n";
  return execution.gpu_enabled();
}
inline bool preview_export(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    ps::ExecutionMode mode) {
  s3::Scene scene(registry, mode);
  ps::ExecutionContext execution(registry, config(mode));
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
  std::cout << "S4Gpu.PreviewExport stamps=" << app.applied_stamps
            << " export_tiles=" << app.export_tiles
            << " preview_tiles=" << app.preview_tiles
            << " native_dispatches=" << app.native_dispatches
            << " rejected=" << app.rejected << " quality=full oracle=passed\n";
  if (mode == ps::ExecutionMode::MetalFp32 && execution.gpu_enabled())
    s3::require(app.native_dispatches > 0,
                "preview/export performed no Metal work");
  return execution.gpu_enabled();
}
}  // namespace s4
