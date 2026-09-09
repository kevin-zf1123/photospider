#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"
#include "s4_gpu_workflow/workflow.hpp"

namespace {
/** @brief Exercises real native shared work and subscriber-owned cancellation.
 */
void sharing(const std::shared_ptr<ps::OperationRegistry>& base) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  ps::CancellationToken observed;
  std::atomic<unsigned> count{0};
  auto traits = base->find_traits("image.exposure_gain").take_value();
  s3::require(registry
                  ->register_operation(
                      {"image.exposure_gain", traits,
                       [&](const ps::OperationInvocation& call) {
                         ++count;
                         {
                           std::unique_lock<std::mutex> lock(mutex);
                           entered = true;
                           observed = call.cancellation;
                           changed.notify_all();
                           if (!changed.wait_for(lock, std::chrono::seconds(10),
                                                 [&] { return release; }))
                             return ps::Result<ps::Value>(ps::Status::failure(
                                 ps::ErrorCode::OperationFailed,
                                 "native gate timeout"));
                         }
                         return base->invoke("image.exposure_gain", call);
                       }})
                  .ok(),
              "gated registry");
  s3::require(registry->freeze().ok(), "gated freeze");
  auto scene = s4_fixture::scene(0);
  ps::InputSnapshotStore snapshots;
  scene.bindings.inputs[0].snapshot = std::make_shared<ps::InputSnapshot>(
      snapshots.import_value(scene.bindings.inputs[0].value).take_value());
  scene.bindings.inputs[0].value = {};
  ps::GraphContext graph(scene.document);
  ps::PlanningOptions options;
  options.execution_mode = ps::ExecutionMode::MetalFp32;
  auto plan = ps::Compiler(registry).compile(graph, options).take_value().plan;
  ps::ExecutionContext execution(registry,
                                 s4::config(ps::ExecutionMode::MetalFp32));
  auto wait_entered = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    s3::require(changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return entered; }),
                "native callback gate timeout");
  };
  auto open_gate = [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    changed.notify_all();
  };
  ps::CancellationSource cancel;
  auto first = std::async(std::launch::async, [&] {
    return execution.execute(plan, scene.bindings, cancel.token());
  });
  wait_entered();
  auto second = std::async(std::launch::async, [&] {
    return execution.execute(plan, scene.bindings);
  });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (execution.cache_statistics().shared_computations == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  if (execution.cache_statistics().shared_computations == 0) {
    open_gate();
    throw std::runtime_error("native flight was not shared");
  }
  cancel.cancel();
  s3::require(first.get().status().code == ps::ErrorCode::Cancelled,
              "cancelled subscriber published");
  s3::require(!observed.cancelled(),
              "one subscriber cancelled shared producer");
  execution.clear_result_cache();
  open_gate();
  auto completed = s3::take(second.get());
  s4_fixture::check(scene, completed.values.at("result"));
  s3::require(count == 1 && execution.cache_statistics().retained_bytes == 0,
              "clear allowed old producer retention");
  {
    std::lock_guard<std::mutex> lock(mutex);
    entered = false;
    release = false;
  }
  ps::CancellationSource last;
  auto pending = std::async(std::launch::async, [&] {
    return execution.execute(plan, scene.bindings, last.token());
  });
  wait_entered();
  last.cancel();
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed.cancelled() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool drained_later = pending.wait_for(std::chrono::milliseconds(0)) !=
                             std::future_status::ready;
  open_gate();
  s3::require(
      pending.get().status().code == ps::ErrorCode::Cancelled && drained_later,
      "last subscriber did not drain callback");
  s3::require(execution.execute(plan, scene.bindings).ok(),
              "context did not recover after cancellation");
}
/** @brief A GPU Whole boundary must carry its CPU fallback ancestry to tiles.
 */
void whole_fallback(const std::shared_ptr<ps::OperationRegistry>& base) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  auto gain = base->find_traits("image.exposure_gain").take_value();
  s3::require(registry
                  ->register_operation(
                      {"test.fallback", gain,
                       [base](const ps::OperationInvocation& call) {
                         if (call.backend == ps::Backend::Gpu)
                           return ps::Result<ps::Value>(ps::Status::failure(
                               ps::ErrorCode::BackendUnavailable,
                               "intentional native fallback"));
                         return base->invoke("image.exposure_gain", call);
                       }})
                  .ok(),
              "fallback registration");
  auto whole = base->find_traits("image.opacity").take_value();
  whole.region_rule = ps::OperationRegionRule::Whole;
  s3::require(
      registry
          ->register_operation({"test.whole", whole,
                                [base](const ps::OperationInvocation& call) {
                                  return base->invoke("image.opacity", call);
                                }})
          .ok(),
      "whole registration");
  s3::require(registry
                  ->register_operation(
                      {"test.final", gain,
                       [base](const ps::OperationInvocation& call) {
                         return base->invoke("image.exposure_gain", call);
                       }})
                  .ok(),
              "final registration");
  s3::require(registry->freeze().ok(), "whole fallback freeze");
  auto image = s1_fixture::value(std::vector<float>(4 * 4 * 4, .5F), {4, 4, 4});
  auto factor = s1_fixture::scalar(.5F);
  ps::WorkflowDocument document;
  document.inputs = {s1_fixture::declaration(1, "image", image),
                     s1_fixture::declaration(2, "factor", factor)};
  document.nodes = {
      {1,
       "test.fallback",
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       {}},
      {2,
       "test.whole",
       {ps::WorkflowNodeOutput{1, "value"}, ps::WorkflowInputReference{2}},
       {}},
      {3,
       "test.final",
       {ps::WorkflowNodeOutput{2, "value"}, ps::WorkflowInputReference{2}},
       {}}};
  document.outputs = {{"result", 3, "value"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.execution_mode = ps::ExecutionMode::MetalFp32;
  planning.tile_height = planning.tile_width = 2;
  auto plan = s3::take(ps::Compiler(registry).compile(graph, planning)).plan;
  ps::InputSnapshotStore store;
  auto snapshot =
      std::make_shared<ps::InputSnapshot>(s3::take(store.import_value(image)));
  ps::ExecutionBindings bindings{
      {{"image", {}, {}, snapshot}, {"factor", factor}}};
  ps::ExecutionContext context(registry,
                               s4::config(ps::ExecutionMode::MetalFp32));
  for (int run = 0; run < 2; ++run) {
    auto result = s3::take(context.execute(plan, bindings));
    unsigned final_calls = 0;
    for (const auto& timing : result.diagnostics.operation_timings)
      if (timing.node_id == 3)
        final_calls += timing.invocation_count;
    s3::require(final_calls == 4 && result.diagnostics.cache_hits == 0 &&
                    result.diagnostics.fallback_reasons.size() == 1 &&
                    result.diagnostics.native_dispatch_count == 5,
                "Whole fallback ancestry entered Metal result cache");
    const auto& value = result.values.at("result");
    for (std::size_t offset = 0; offset < value.bytes().size(); offset += 4) {
      float actual;
      std::memcpy(&actual, value.bytes().data() + offset, 4);
      s3::require(actual == (offset % 16 == 12 ? .25F : .0625F),
                  "Whole fallback oracle mismatch");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    auto registry = argc > 1 ? std::make_shared<ps::OperationRegistry>()
                             : ps::make_default_operation_registry();
    if (argc > 1) {
      s3::require(registry->load_plugin(argv[1]).ok(), "plugin load");
      s3::require(registry->freeze().ok(), "registry freeze");
    }
    if (!s4::cache_edits(registry, ps::ExecutionMode::MetalFp32))
      return 77;
    sharing(registry);
    whole_fallback(registry);
    if (!registry->persistent_cache_identity().empty()) {
      const auto directory =
          std::filesystem::temp_directory_path() /
          ("photospider-s4-disk-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
      struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
          std::error_code error;
          std::filesystem::remove_all(directory, error);
        }
      } cleanup{directory};
      auto config = s4::config(ps::ExecutionMode::MetalFp32);
      config.disk_cache =
          ps::DiskCacheConfig{directory.string(), 1024 * 1024, 4096, 512};
      ps::ExecutionContext context(registry, config);
      s3::Scene native(registry, ps::ExecutionMode::MetalFp32);
      s3::require(context.execute(native.freeze(context, 2)).ok(),
                  "native disk isolation run");
      context.flush_disk_cache();
      s3::require(context.disk_cache_statistics().entries == 0,
                  "Metal ancestry reached disk cache");
      s3::Scene exact(registry);
      s3::require(context.execute(exact.freeze(context, 2)).ok(),
                  "CPU disk run");
      context.flush_disk_cache();
      s3::require(context.disk_cache_statistics().entries > 0,
                  "CPU disk cache stopped working");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
