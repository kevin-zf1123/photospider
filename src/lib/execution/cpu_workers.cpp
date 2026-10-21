#include "execution/cpu_workers.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ps::execution_internal {
std::uint64_t CpuWorkers::metadata_bytes(
    std::uint32_t count, std::uint32_t queue_capacity) noexcept {
  return sizeof(CpuWorkers) + std::uint64_t{count} * sizeof(std::thread) +
         std::uint64_t{queue_capacity} * sizeof(CpuWork);
}
CpuWorkers::CpuWorkers(std::uint32_t count, std::uint32_t queue_capacity,
                       ResourceLease metadata)
    : allocation_(std::move(metadata)) {
  if (!count || count > 4096 || !queue_capacity)
    throw std::invalid_argument("invalid CPU worker or queue capacity");
  queue_.resize(queue_capacity);
  allocation_.commit(queue_.capacity() * sizeof(CpuWork));
  workers_.reserve(count);
  allocation_.commit(workers_.capacity() * sizeof(std::thread));
  try {
    for (std::uint32_t i = 0; i < count; ++i)
      workers_.emplace_back([this] { worker(); });
  } catch (...) {
    close();
    throw;
  }
}
CpuWorkers::~CpuWorkers() noexcept {
  close();
}

CpuSubmission CpuWorkers::submit(CpuWork work) {
  if (!work.invoke || !work.complete)
    return CpuSubmission::Invalid;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_)
      return CpuSubmission::Closed;
    if (count_ == queue_.size())
      return CpuSubmission::Busy;
    queue_[(head_ + count_) % queue_.size()] = work;
    ++count_;
  }
  ready_.notify_one();
  return CpuSubmission::Accepted;
}

CpuSubmission CpuWorkers::submit_ready(void* context, AcquireWork acquire) {
  if (!acquire)
    return CpuSubmission::Invalid;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_)
      return CpuSubmission::Closed;
    if (count_ == queue_.size())
      return CpuSubmission::Busy;
    auto work = acquire(context);
    if (!work.invoke || !work.complete)
      return CpuSubmission::Invalid;
    queue_[(head_ + count_) % queue_.size()] = work;
    ++count_;
  }
  ready_.notify_one();
  return CpuSubmission::Accepted;
}

void CpuWorkers::worker() noexcept {
  for (;;) {
    CpuWork work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return count_ || closing_; });
      if (!count_)
        return;
      work = queue_[head_];
      queue_[head_] = {};
      head_ = (head_ + 1) % queue_.size();
      --count_;
      peak_ = std::max(peak_, ++active_);
    }
    work.invoke(work.context);
    work.complete(work.context);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_;
      if (!active_ && !count_)
        idle_.notify_all();
    }
  }
}

void CpuWorkers::drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return !count_ && !active_; });
}
std::size_t CpuWorkers::peak_active() {
  std::lock_guard<std::mutex> lock(mutex_);
  return peak_;
}
void CpuWorkers::close() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable())
      worker.join();
}
}  // namespace ps::execution_internal
