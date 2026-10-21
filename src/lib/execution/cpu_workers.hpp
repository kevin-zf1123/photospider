#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "core/resource_observation.hpp"

namespace ps::execution_internal {
/** @brief Preallocated invocation and completion pair borrowed through drain.
 * invoke catches operation exceptions before returning. complete publishes one
 * reserved terminal record after invoke has ended every payload access. Both
 * callbacks must terminate and are noexcept; neither starts nested pool work.
 */
struct CpuWork final {
  void* context = nullptr;
  void (*invoke)(void* context) noexcept = nullptr;
  void (*complete)(void* context) noexcept = nullptr;
};
using AcquireWork = CpuWork (*)(void*) noexcept;  // NOLINT(readability/casting)

enum class CpuSubmission : std::uint8_t { Accepted, Busy, Closed, Invalid };

/** @brief Context-owned bounded CPU workers for fixed physical invocations.
 * The FIFO and worker storage allocate at construction. submit allocates
 * nothing. Accepted work invokes both callbacks exactly once, including during
 * shutdown; rejection invokes neither callback and transfers no borrowing.
 * Independent Run coordinators may submit concurrently. drain/destruction
 * must run outside this pool's callbacks and after submitters have stopped.
 */
class CpuWorkers final {
 public:
  static std::uint64_t metadata_bytes(std::uint32_t count,
                                      std::uint32_t queue_capacity) noexcept;
  CpuWorkers(std::uint32_t count, std::uint32_t queue_capacity,
             ResourceLease metadata = {});
  ~CpuWorkers() noexcept;
  CpuWorkers(const CpuWorkers&) = delete;
  CpuWorkers& operator=(const CpuWorkers&) = delete;
  CpuSubmission submit(CpuWork work);
  /** @brief Claim one ready invocation only when queue capacity is available.
   * acquire runs under the queue lock and must not allocate, block or throw.
   * An empty descriptor means no ready work. Busy/Closed never call acquire.
   */
  CpuSubmission submit_ready(void* context, AcquireWork acquire);
  void drain();
  /** @brief Stop new submissions and join every worker after accepted drain.
   * Call once after all Run coordinators have stopped submitting.
   */
  void shutdown() noexcept { close(); }
  std::size_t peak_active();

 private:
  void worker() noexcept;
  void close() noexcept;
  core_internal::MetadataAllocation allocation_;
  std::mutex mutex_;
  std::condition_variable ready_, idle_;
  std::vector<CpuWork> queue_;
  std::vector<std::thread> workers_;
  std::size_t head_ = 0, count_ = 0, active_ = 0, peak_ = 0;
  bool closing_ = false;
};
}  // namespace ps::execution_internal
