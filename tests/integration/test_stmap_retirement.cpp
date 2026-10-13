#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string_view>
#include <vector>

#include "support/execution_sync_fixture.hpp"
#include "support/stmap_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
struct CallbackGate final {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false, timed_out = false;
  void hold() noexcept {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(20),
                          [&] { return released; }))
      timed_out = true;
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(10),
                            [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
CallbackGate* cancellation_gate = nullptr;
void hold_callback_retirement() noexcept {
  cancellation_gate->hold();
}
struct HookScope final {
  explicit HookScope(const execution_testing::ExecutionTestHooks& hooks) {
    execution_testing::install_execution_test_hooks(&hooks);
  }
  ~HookScope() { execution_testing::install_execution_test_hooks(nullptr); }
};
template <class Run>
int cancel_at_retirement(const ResourceBudget& root, Run run,
                         bool staged = false) {
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  const auto work = root.statistics().issued.work;
  CallbackGate gate;
  cancellation_gate = &gate;
  execution_testing::ExecutionTestHooks hooks;
  if (staged)
    hooks.final_result_ready = hold_callback_retirement;
  else
    hooks.callback_body_finished = hold_callback_retirement;
  HookScope installed(hooks);
  CancellationSource stop;
  std::future<decltype(run(stop.token()))> active;
  ps::test::OnExit cleanup([&] {
    stop.cancel();
    gate.release();
    if (active.valid())
      active.wait();
    cancellation_gate = nullptr;
  });
  active = std::async(std::launch::async, [&] { return run(stop.token()); });
  const bool entered = gate.wait();
  stop.cancel();
  gate.release();
  auto interrupted = active.get();
  PS_CHECK(entered && !gate.timed_out);
  PS_CHECK(root.statistics().issued.work > work);
  PS_CHECK(!interrupted.ok());
  if (interrupted.status().code != ErrorCode::Cancelled) {
    ps::test::require_ok(interrupted, "cancelled sampling callback", __FILE__,
                         __LINE__);
    return 1;
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}
template <class Run>
int cancel_in_loop(const ResourceBudget& root, std::string_view operation,
                   Run run) {
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  ps::test::PhaseWorkGate gate(operation, 8);
  CancellationSource stop;
  std::future<decltype(run(stop.token()))> active;
  ps::test::OnExit cleanup([&] {
    stop.cancel();
    gate.release();
    if (active.valid())
      active.wait();
  });
  active = std::async(std::launch::async, [&] { return run(stop.token()); });
  const bool entered = gate.wait();
  stop.cancel();
  gate.release();
  auto interrupted = active.get();
  PS_CHECK(entered && gate.cancelled_in_service());
  PS_CHECK(!interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}

int cancellation() {
  stmap_test::StmapDriver driver;
  const auto source =
      driver.source(stmap_test::image_schema(1, 1), {1, 2, 3, 1});
  const auto map =
      driver.map(std::vector<double>(2 * 16 * 16, .5), {16, 16, 2});
  const auto output = take(Footprint::all({1, 1, 16, 16, 4}));
  const auto run = [&](const CancellationToken& token) {
    return driver.run(source, map, "clamp", output, {}, token);
  };
  PS_REQUIRE_OK(run({}));
  return cancel_at_retirement(driver.root, run) ||
         cancel_at_retirement(driver.root, run, true) ||
         cancel_in_loop(driver.root, "image.stmap", run);
}
}  // namespace
int main() {
  return cancellation();
}
