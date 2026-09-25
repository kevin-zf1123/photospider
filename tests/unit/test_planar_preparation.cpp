#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "support/fmt_handoff.hpp"

int main() {
  using namespace ps;                   // NOLINT(build/namespaces)
  using namespace ps::handoff_testing;  // NOLINT(build/namespaces)
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  take(registry->register_operation(probe_operation(probe)));
  take(registry->freeze());
  const auto image = probe_image();
  auto document = probe_document(image);
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  require(probe->preparations == 1, "compile must prepare exactly once");
  ExecutionContext execution(registry);
  for (unsigned i = 0; i < 3; ++i)
    take(execution.execute(compiled.plan, probe_bindings(image)));
  require(probe->preparations == 1 && probe->callbacks == 3,
          "planar execution repeated static preparation");
  std::array<bool, 2> success{};
  std::thread a([&] {
    success[0] = execution.execute(compiled.plan, probe_bindings(image)).ok();
  });
  std::thread b([&] {
    success[1] = execution.execute(compiled.plan, probe_bindings(image)).ok();
  });
  a.join();
  b.join();
  require(success[0] && success[1] && probe->preparations == 1,
          "concurrent runs did not share immutable preparation");

  // Exercise the shared seal gate via public staged requests; no private-test
  // access or weakened invocation validator is introduced.
  OperationMetadata metadata;
  metadata.descriptor = image.descriptor();
  metadata.planar_layout = document.inputs[0].planar_layout;
  const auto& params = document.nodes[0].parameters;
  auto prepared = take(
      registry->prepare_operation("review.transfer_probe", {metadata}, params));
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto changed = metadata;
    switch (mutation) {
      case 0:
        changed.planar_layout.reset();
        break;
      case 1:
        changed.planar_layout->order = ImagePlaneOrder::Continuous;
        break;
      case 2:
        std::swap(changed.planar_layout->height_axis,
                  changed.planar_layout->width_axis);
        break;
      case 3:
        changed.planar_layout->channel_axis.reset();
        break;
      case 4:
        changed.planar_layout->row_pitch_bytes = 64;
        break;
      case 5:
        changed.planar_layout->groups = {{"extra", 0, 1}};
        break;
      case 6:
        changed.descriptor.shape[0] += 1;
        break;
    }
    DependencyRequest request;
    request.inputs = {changed};
    request.parameters = params;
    request.outputs = take(Footprint::all(metadata.descriptor.shape));
    request.snapshot_identity = "handoff-seal";
    request.prepared = prepared;
    auto result = registry->start_dependency("review.transfer_probe", request);
    require(!result.ok() && result.status().code == ErrorCode::Stale,
            "changed metadata accepted by preparation seal");
  }
  // Cancellation/currentness must prevent successful completion after callback.
  CancellationSource stop;
  probe->on_callback = [&] { stop.cancel(); };
  auto cancelled =
      execution.execute(compiled.plan, probe_bindings(image), stop.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "midcallback cancellation escaped publication gate");
  probe->on_callback = [&] { static_cast<void>(graph.replace(document)); };
  auto stale = execution.execute(compiled.plan, probe_bindings(image));
  require(!stale.ok() && stale.status().code == ErrorCode::Stale,
          "changed graph escaped publication gate");
  probe->on_callback = {};
  // Hold a callback after it has written the complete unpublished ROI. Stop
  // or invalidate from a different thread, without sleeps or timing guesses.
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto current_plan = take(Compiler(registry).compile(graph));
    std::mutex mutex;
    std::condition_variable changed;
    bool copied = false, release = false;
    probe->after_copy = [&] {
      std::unique_lock<std::mutex> lock(mutex);
      copied = true;
      changed.notify_one();
      changed.wait(lock, [&] { return release; });
    };
    CancellationSource concurrent_stop;
    ErrorCode code = ErrorCode::Ok;
    std::thread runner([&] {
      auto result = execution.execute(current_plan.plan, probe_bindings(image),
                                      concurrent_stop.token());
      code = result.status().code;
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      changed.wait(lock, [&] { return copied; });
    }
    if (mode != 1)
      concurrent_stop.cancel();
    if (mode != 0)
      static_cast<void>(graph.replace(document));
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    changed.notify_one();
    runner.join();
    require(code == (mode == 1 ? ErrorCode::Stale : ErrorCode::Cancelled),
            "concurrent stop escaped post-copy publication fence");
    probe->after_copy = {};
    auto recovered = take(Compiler(registry).compile(graph));
    take(execution.execute(recovered.plan, probe_bindings(image)));
  }
  return 0;
}
