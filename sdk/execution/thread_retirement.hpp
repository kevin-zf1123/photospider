#pragma once
#include <memory>
#include <thread>
namespace ps::executor_sdk {
class ThreadJoiner;
/** @brief Reserved when a dynamic worker is created. The host can join the
 * worker after a self/cross-context shutdown without allocating on teardown. */
struct ThreadRetirement final {
  std::shared_ptr<const void> module;
  std::thread thread;
  std::unique_ptr<ThreadRetirement> next;
  ThreadJoiner* joiner = nullptr;
};
std::unique_ptr<ThreadRetirement> reserve_thread_retirement(
    std::shared_ptr<const void> module);
void retire_thread(std::thread worker,
                   std::unique_ptr<ThreadRetirement> record) noexcept;
}  // namespace ps::executor_sdk
