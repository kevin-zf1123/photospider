#include "sdk/execution/thread_retirement.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
namespace ps::executor_sdk {
class ThreadJoiner final {
 public:
  ThreadJoiner() : worker_([this] { run(); }) {}
  ~ThreadJoiner() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    changed_.notify_one();
    worker_.join();
  }
  void retire(std::unique_ptr<ThreadRetirement> record) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      record->next = std::move(pending_);
      pending_ = std::move(record);
    }
    changed_.notify_one();
  }

 private:
  void run() {
    for (;;) {
      std::unique_ptr<ThreadRetirement> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return pending_ || closing_; });
        if (!pending_)
          return;
        job = std::move(pending_);
        pending_ = std::move(job->next);
      }
      job->thread.join();
    }
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<ThreadRetirement> pending_;
  bool closing_ = false;
  std::thread worker_;
};
std::unique_ptr<ThreadRetirement> reserve_thread_retirement(
    std::shared_ptr<const void> module) {
  if (!module)
    return {};
  // Embedded hosts can destroy contexts from static destructors. Keep the
  // process-wide join service available through the last such retirement.
  static ThreadJoiner* const joiner = new ThreadJoiner;
  auto record = std::make_unique<ThreadRetirement>();
  record->module = std::move(module);
  record->joiner = joiner;
  return record;
}
void retire_thread(std::thread worker,
                   std::unique_ptr<ThreadRetirement> record) noexcept {
  if (!worker.joinable())
    return;
  if (!record) {
    worker.detach();
    return;
  }
  record->thread = std::move(worker);
  auto* joiner = record->joiner;
  joiner->retire(std::move(record));
}
}  // namespace ps::executor_sdk
