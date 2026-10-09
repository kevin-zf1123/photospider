#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#include "core/checked_math.hpp"
#include "execution/cpu_range.hpp"
#include "photospider/plugin/cpu_tiles_api.h"

namespace ps::execution_internal {
/** @brief Invocation-local coordinator service for sequential tiled stages. */
class CpuTileScope final {
 public:
  CpuTileScope(CpuRangeQueue& queue, const CancellationToken& cancellation,
               const ResourceBudget* resources, std::uint32_t workers,
               std::function<bool()> current)
      : queue_(queue),
        cancellation_(cancellation),
        resources_(resources),
        current_(std::move(current)),
        owner_(std::this_thread::get_id()),
        service_{sizeof(service_), PS_CPU_TILES_ABI_VERSION_1,
                 std::min(workers, queue.workers()), this, invoke} {}
  const ps_cpu_tiles_service_v1* service() const noexcept { return &service_; }
  Status status() const {
    return violation_.load() ? Status{ErrorCode::InvalidArgument,
                                      "CPU tile coordinator thread violation"}
                             : failure_;
  }
  std::uint64_t stages() const noexcept { return stages_; }
  std::uint64_t tiles() const noexcept { return tiles_; }
  std::uint32_t peak() const noexcept { return peak_; }

 private:
  struct Work {
    ps_cpu_tile_stage_v1 stage;
    std::uint64_t counts[3];
    ps_cpu_tile_callback_v1 callback;
    void* user;
    const std::atomic<bool>* violation;
  };
  static int execute(void* raw, std::uint64_t index, std::uint64_t,
                     std::uint32_t slot) {
    const auto& work = *static_cast<const Work*>(raw);
    if (work.violation->load())
      return 6;
    ps_cpu_tile_v1 tile{};
    tile.struct_size = sizeof(tile);
    tile.slot = slot;
    tile.index = index;
    for (unsigned axis = 0; axis < 3; ++axis) {
      tile.begin[axis] = (index % work.counts[axis]) * work.stage.tile[axis];
      index /= work.counts[axis];
      tile.end[axis] = tile.begin[axis] +
                       std::min(work.stage.tile[axis],
                                work.stage.extent[axis] - tile.begin[axis]);
    }
    const int result = work.callback(work.user, &tile);
    return work.violation->load() ? 6 : result;
  }
  Status run(const ps_cpu_tile_stage_v1* stage,
             ps_cpu_tile_callback_v1 callback, void* user) {
    if (!stage || stage->struct_size != sizeof(*stage) || !callback ||
        !service_.maximum_workers || stage->workers > service_.maximum_workers)
      return Status{ErrorCode::InvalidArgument, "invalid CPU tile stage"};
    Work work{*stage, {}, callback, user, &violation_};
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto size = stage->extent[axis], grain = stage->tile[axis];
      if (!grain)
        return Status{ErrorCode::InvalidArgument, "zero CPU tile extent"};
      work.counts[axis] = size / grain + (size % grain != 0);
    }
    if (work.counts[0] && work.counts[1] && work.counts[2]) {
      for (auto dimension : work.counts) {
        if (!core_internal::can_multiply(count, dimension))
          return Status{ErrorCode::ResourceExhausted,
                        "CPU tile count overflow"};
        count *= dimension;
      }
    } else {
      count = 0;
    }
    if (!core_internal::can_multiply_add(count, 256, 256) ||
        !core_internal::can_add(tiles_, count) || stages_ == UINT64_MAX)
      return Status{ErrorCode::ResourceExhausted,
                    "CPU tile accounting overflow"};
    ResourceLease metadata;
    if (resources_) {
      auto owned = resources_->reserve(
          ResourceCapacity::host(sizeof(Work), sizeof(Work)));
      if (!owned.ok())
        return owned.status();
      metadata = owned.take_value();
      auto charged = resources_->consume({256 + 256 * count});
      if (!charged.ok())
        return charged;
    }
    std::uint32_t peak = 0;
    auto status = queue_.run_external(
        count, stage->workers ? stage->workers : service_.maximum_workers,
        execute, &work, cancellation_, resources_, current_, &peak);
    if (status.ok()) {
      ++stages_;
      tiles_ += count;
    }
    peak_ = std::max(peak_, peak);
    return status;
  }
  static int invoke(void* raw, const ps_cpu_tile_stage_v1* stage,
                    ps_cpu_tile_callback_v1 callback, void* user) noexcept {
    auto& self = *static_cast<CpuTileScope*>(raw);
    if (std::this_thread::get_id() != self.owner_ || in_kernel_worker ||
        in_cpu_range) {
      self.violation_.store(true);
      return 6;
    }
    try {
      if (self.violation_.load())
        self.failure_.code = ErrorCode::InvalidArgument;
      if (self.failure_.ok())
        self.failure_ = self.run(stage, callback, user);
      if (self.violation_.load())
        self.failure_.code = ErrorCode::InvalidArgument;
    } catch (const std::bad_alloc&) {
      self.failure_.code = ErrorCode::ResourceExhausted;
    } catch (...) {
      self.failure_.code = ErrorCode::OperationFailed;
    }
    return self.failure_.ok()                                    ? 0
           : self.failure_.code == ErrorCode::Cancelled          ? 2
           : self.failure_.code == ErrorCode::BackendUnavailable ? 3
           : self.failure_.code == ErrorCode::ResourceExhausted  ? 4
           : self.failure_.code == ErrorCode::TypeMismatch       ? 5
           : self.failure_.code == ErrorCode::InvalidArgument    ? 6
                                                                 : 1;
  }
  CpuRangeQueue& queue_;
  CancellationToken cancellation_;
  const ResourceBudget* resources_;
  std::function<bool()> current_;
  const std::thread::id owner_;
  ps_cpu_tiles_service_v1 service_;
  Status failure_;
  std::atomic<bool> violation_{false};
  std::uint64_t stages_ = 0, tiles_ = 0;
  std::uint32_t peak_ = 0;
};
}  // namespace ps::execution_internal
