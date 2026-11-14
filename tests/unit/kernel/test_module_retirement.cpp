#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "photospider/core/resources.hpp"
#include "sdk/execution/thread_retirement.hpp"
#include "support/status_check.hpp"

namespace {
struct ExitProbe {
  std::atomic<bool>* finished;
  ~ExitProbe() { finished->store(true); }
};
}  // namespace
int main() {
  std::atomic<bool> finished{false}, released{false}, ordered{false};
  auto module = std::shared_ptr<const void>(&finished, [&](const void*) {
    ordered.store(finished.load());
    released.store(true);
  });
  ps::ResourceBudget root({}, module);
  module.reset();
  auto record = ps::executor_sdk::reserve_thread_retirement(root.code_owner());
  std::thread worker([&] {
    thread_local ExitProbe exit{&finished};
    (void)exit;
  });
  ps::executor_sdk::retire_thread(std::move(worker), std::move(record));
  root = ps::ResourceBudget{};
  for (unsigned i = 0; i < 10000 && !released.load(); ++i)
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  PS_CHECK(released.load() && ordered.load());
  return 0;
}
