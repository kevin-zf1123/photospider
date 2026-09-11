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
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"
#include "s4_gpu_workflow/workflow.hpp"
#include "support/typed_images.hpp"

namespace {
/** @brief Pure public-ABI byte copy exercises arbitrary typed image residency.
 */
void typed_residency() {
  auto operations = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits traits;
  traits.input_count = 1;
  traits.input_schema = {{ps::OperationPortKind::Typed}};
  traits.input_schema[0].semantic_kind =
      static_cast<std::uint32_t>(ps::SemanticKind::Image);
  traits.outputs[0].output_dtype_rule = ps::OperationDtypeRule::Input;
  traits.outputs[0].output_semantic_rule =
      ps::OperationSemanticRule::PreserveInput;
  traits.outputs[0].shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  traits.supports_gpu = true;
  traits.allows_cpu_fallback = false;
  s3::require(
      operations
          ->register_operation(
              {"core.identity", traits,
               [](const ps::OperationInvocation& call)
                   -> ps::Result<ps::Value> {
                 auto made = ps::MutableValue::allocate(
                     call.inputs[0].descriptor(), call.output_region,
                     call.allocator);
                 if (!made.ok())
                   return ps::Result<ps::Value>(made.status());
                 auto output = made.take_value();
                 std::uint64_t source = 0, destination = 0;
                 const auto* gpu = call.gpu;
                 if (!gpu ||
                     gpu->buffer(gpu->context, call.inputs[0].bytes().data(),
                                 call.inputs[0].bytes().size(), 0, &source) ||
                     gpu->buffer(gpu->context, output.data(), output.size(), 1,
                                 &destination))
                   return ps::Result<ps::Value>(ps::Status::failure(
                       ps::ErrorCode::OperationFailed, "typed native buffers"));
                 const char shader[] =
                     "#include <metal_stdlib>\nusing namespace metal;\n"
                     "kernel void copy_bits(device const uint* a "
                     "[[buffer(0)]], device uint* b [[buffer(1)]], uint i "
                     "[[thread_position_in_grid]]) { b[i]=a[i]; }";
                 ps_gpu_buffer_binding_v9 buffers[] = {
                     {sizeof(ps_gpu_buffer_binding_v9), 0, source, 0,
                      output.size(), 0},
                     {sizeof(ps_gpu_buffer_binding_v9), 1, destination, 0,
                      output.size(), 1}};
                 ps_gpu_dispatch_v9 command{};
                 command.struct_size = sizeof(command);
                 command.source = shader;
                 command.source_size = sizeof(shader) - 1;
                 command.entry = "copy_bits";
                 command.entry_size = 9;
                 command.buffers = buffers;
                 command.buffer_count = 2;
                 command.grid[0] = output.size() / 4;
                 command.grid[1] = command.grid[2] = 1;
                 if (gpu->execute(gpu->context, &command, 1))
                   return ps::Result<ps::Value>(ps::Status::failure(
                       ps::ErrorCode::OperationFailed, "typed native copy"));
                 return std::move(output).publish(call.inputs[0].facets());
               }})
          .ok(),
      "typed native registration");
  s3::require(operations->freeze().ok(), "typed registry freeze");
  ps::Compiler compiler(operations);
  ps::InputSnapshotStore store;
  ps::ExecutionContext execution(operations,
                                 s4::config(ps::ExecutionMode::MetalFp32));
  ps::PlanningOptions planning;
  planning.execution_mode = ps::ExecutionMode::MetalFp32;
  for (const auto& semantic : typed_images::descriptions()) {
    auto original = typed_images::value(semantic);
    ps::GraphContext graph(typed_images::document(original));
    auto plan = s3::take(compiler.compile(graph, planning)).plan;
    ps::ExecutionBindings bindings{
        {{"image",
          {},
          {},
          std::make_shared<ps::InputSnapshot>(
              s3::take(store.import_value(original)))}}};
    auto cold = s3::take(execution.execute(plan, bindings));
    s3::require(typed_images::same(cold.values.at("result"), original) &&
                    cold.diagnostics.native_dispatch_count == 1 &&
                    cold.diagnostics.fallback_reasons.empty(),
                "typed native cold oracle");
    auto warm = s3::take(execution.execute(plan, bindings));
    s3::require(typed_images::same(warm.values.at("result"), original) &&
                    warm.diagnostics.native_dispatch_count == 0 &&
                    warm.diagnostics.cache_hits > 0,
                "typed native cached oracle");
  }
  s3::require(execution.cache_statistics().native_retained_bytes > 0,
              "typed images did not retain native owners");
}
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
  whole.outputs[0].region_rule = ps::OperationRegionRule::Whole;
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
    typed_residency();
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
