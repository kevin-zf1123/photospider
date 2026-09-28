#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "execution/native_gpu_metadata.hpp"
#include "photospider/data/storage.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/plugin/operation_plugin_api.h"

namespace ps::gpu_internal {
/** @brief Per-invocation real native work observations. */
struct Statistics final {
  std::uint64_t dispatches = 0;
  std::uint64_t submissions = 0;
  std::uint64_t device_us = 0;
  std::uint64_t constant_bytes = 0;
};
/** @brief A retained bounded binding, private to one synchronous invocation. */
struct BufferView final {
  std::shared_ptr<const CpuStorage> storage;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  bool writable = false;
  std::uint32_t generation = 0;
};
/** @brief Returns native payload capacity, or zero on overflow. */
std::uint64_t allocation_capacity(std::uint64_t bytes) noexcept;

/** @brief Optional process-local GPU device with no exported native handles.
 * @note Queue operations serialize and complete before return. Allocations
 * retain their native owner independently. Creation returns null if
 * unavailable.
 */
class Device final : public std::enable_shared_from_this<Device> {
 public:
  static std::shared_ptr<Device> create(
      std::shared_ptr<ResourceBudget> budget = {});
  std::shared_ptr<MetadataAccount> metadata() const;
  void clear_pipeline_cache();
  void collect_expired_allocations();
  ~Device();
  bool available() const noexcept;
  std::uint32_t backend() const noexcept;
  std::uint64_t minimum_buffer_offset_alignment() const noexcept;
  /** @brief Device-resolved allocation capacity for this exact logical size. */
  Result<std::uint64_t> allocation_capacity(std::uint64_t bytes);
  std::string identity() const;
  BufferAllocator allocator(const BufferAllocator& host);
  bool owns(const CpuStorage& storage) const noexcept;
  Result<BufferView> view(const std::uint8_t* bytes, std::uint64_t size,
                          bool writable);
  Status execute(
      const std::vector<BufferView, NativeAllocator<BufferView>>& views,
      const ps_gpu_dispatch_v11* commands, std::uint32_t count,
      const CancellationToken& cancellation, Statistics* statistics,
      const BufferAllocator& command_allocator);

 private:
  struct Impl;
  explicit Device(std::unique_ptr<Impl> impl);
  Result<MutableBuffer> allocate(std::uint64_t size,
                                 const BufferAllocator::Reserve& reserve,
                                 std::shared_ptr<const void> domain);
  std::unique_ptr<Impl> impl_;
};

/** @brief Pure-C service adapter retaining every acquired token until return.
 */
class Invocation final {
 public:
  Invocation(std::shared_ptr<Device> device, CancellationToken cancellation,
             BufferAllocator command_allocator = BufferAllocator());
  const ps_gpu_service_v11* service() const noexcept { return &service_; }
  const Status& status() const noexcept {
    return thread_violation_.load() ? thread_failure_ : status_;
  }
  const Statistics& statistics() const noexcept { return statistics_; }

 private:
  static int buffer(void* context, const std::uint8_t* bytes,
                    std::uint64_t size, std::uint32_t writable,
                    std::uint64_t* token) noexcept;
  static int execute(void* context, const ps_gpu_dispatch_v11* commands,
                     std::uint32_t count) noexcept;
  static int release(void* context, std::uint64_t token) noexcept;
  bool on_owner_thread() noexcept;
  int fail(Status status) noexcept;
  std::shared_ptr<Device> device_;
  CancellationToken cancellation_;
  BufferAllocator command_allocator_;
  ps_gpu_service_v11 service_{};
  std::vector<BufferView, NativeAllocator<BufferView>> views_;
  Status status_;
  Statistics statistics_;
  const std::thread::id owner_ = std::this_thread::get_id();
  std::atomic<bool> thread_violation_{false};
  const Status thread_failure_{ErrorCode::InvalidArgument,
                               "native GPU service thread violation"};
};
}  // namespace ps::gpu_internal
