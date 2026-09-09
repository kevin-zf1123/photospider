#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
