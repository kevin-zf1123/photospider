/**
 * @file resource_demand_estimator.hpp
 * @brief Declares checked structural retained-memory estimators for CPU Runs.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "compute/task_graph_planning.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"

namespace ps::compute {

struct HighPrecisionDirtyPlan;
struct RealTimeDirtyPlan;

/**
 * @brief Accumulates one structural byte estimate without unsigned overflow.
 *
 * Product adapters use this helper only for Host-owned C++ objects, container
 * capacity, callback captures, and shared-allocation control blocks whose
 * structure is known before Run admission. Any addition or multiplication
 * overflow fails closed with `GraphErrc::ComputeError`.
 *
 * @throws GraphError when an added byte count cannot be represented.
 * @note Image pixels and opaque backend, device, plugin, or allocator-private
 * storage are outside this estimator. Callers must not invent byte counts for
 * storage whose allocation contract is unavailable.
 */
class RetainedMemoryEstimator final {
 public:
  /**
   * @brief Starts an empty estimate for one named adapter boundary.
   * @param boundary Stable diagnostic name retained only as a borrowed literal.
   * @throws Nothing.
   * @note `boundary` must outlive the estimator; callers pass string literals.
   */
  explicit RetainedMemoryEstimator(const char* boundary) noexcept
      : boundary_(boundary) {}

  /**
   * @brief Adds an already-computed byte count with overflow checking.
   * @param bytes Structural bytes to add.
   * @return Nothing.
   * @throws GraphError when the addition would overflow `std::uint64_t`.
   */
  void add_bytes(std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - bytes_) {
      throw GraphError(
          GraphErrc::ComputeError,
          std::string(boundary_) + " retained-memory estimate overflow.");
    }
    bytes_ += bytes;
  }

  /**
   * @brief Adds storage for a repeated complete C++ object type.
   * @tparam Value Complete object type whose `sizeof` is auditable.
   * @param count Number of contiguous or node-contained values.
   * @return Nothing.
   * @throws GraphError when multiplication or accumulation overflows.
   * @note This accounts object storage only; nested owned allocations require
   * separate additions.
   */
  template <typename Value>
  void add_objects(std::uint64_t count = 1U) {
    constexpr std::uint64_t kObjectBytes =
        static_cast<std::uint64_t>(sizeof(Value));
    if (kObjectBytes != 0U &&
        count > std::numeric_limits<std::uint64_t>::max() / kObjectBytes) {
      throw GraphError(
          GraphErrc::ComputeError,
          std::string(boundary_) + " retained-memory estimate overflow.");
    }
    add_bytes(kObjectBytes * count);
  }

  /**
   * @brief Adds the conservative control payload for one shared allocation.
   * @return Nothing.
   * @throws GraphError when accumulation overflows.
   * @note The estimate uses two pointer-width words for the implementation's
   * shared strong/weak ownership bookkeeping. Allocator-private metadata
   * remains outside the declared boundary.
   */
  void add_shared_control_block() { add_objects<void*>(2U); }

  /**
   * @brief Returns the checked accumulated byte count.
   * @return Exact sum of every explicitly added structural component.
   * @throws Nothing.
   */
  std::uint64_t bytes() const noexcept { return bytes_; }

 private:
  /** @brief Borrowed diagnostic boundary name. */
  const char* boundary_;

  /** @brief Checked structural byte sum. */
  std::uint64_t bytes_ = 0U;
};

/**
 * @brief Estimates dynamic storage owned by one `ComputePlan` value.
 * @param plan Plan whose vector capacities and nested payloads are inspected.
 * @return Checked dynamic byte estimate excluding `sizeof(ComputePlan)`.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note The estimate includes task/dependency/ROI container capacity and owned
 * strings. It excludes borrowed Graph state and operation-produced output.
 */
std::uint64_t compute_plan_dynamic_retained_memory_bytes(
    const ComputePlan& plan);

/**
 * @brief Estimates dynamic storage owned by one dirty selection overlay.
 * @param selection Overlay whose capacities, buckets, and nested vectors are
 * inspected.
 * @return Checked dynamic bytes excluding the overlay object's inline size.
 * @throws GraphError when checked structural arithmetic overflows.
 */
std::uint64_t dirty_selection_dynamic_retained_memory_bytes(
    const DirtyTaskSelectionOverlay& selection);

/**
 * @brief Estimates complete Host-owned HP dirty-plan storage.
 * @param plan Plan whose entries, snapshot, and nested capacities are charged.
 * @return Checked inline and dynamic structural bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 */
std::uint64_t high_precision_dirty_plan_retained_memory_bytes(
    const HighPrecisionDirtyPlan& plan);

/**
 * @brief Estimates complete Host-owned RT dirty-plan storage.
 * @param plan Plan whose entries, snapshot, and nested capacities are charged.
 * @return Checked inline and dynamic structural bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 */
std::uint64_t real_time_dirty_plan_retained_memory_bytes(
    const RealTimeDirtyPlan& plan);

/**
 * @brief Estimates visible Host-owned dynamic storage inside one node output.
 * @param output Output whose named values and diagnostic string are inspected.
 * @return Checked dynamic bytes excluding `sizeof(NodeOutput)`.
 * @throws GraphError when recursive named-value arithmetic overflows.
 * @note Pixel payloads, backend contexts, plugin leases, deleter captures, and
 * allocation capacity hidden behind shared pointers are intentionally
 * excluded because the Host cannot audit their allocation size.
 */
std::uint64_t node_output_dynamic_retained_memory_bytes(
    const NodeOutput& output);

/**
 * @brief Returns a conservative owned `std::function` capture allocation.
 * @param capture_bytes Compile-time size of the adapter-owned capture object.
 * @return Capture plus shared/allocation bookkeeping bytes.
 * @throws GraphError when checked addition overflows.
 * @note This is used only when trusted adapter code owns the capture type. It
 * is not an estimate of plugin or backend callbacks.
 */
std::uint64_t owned_callable_retained_memory_bytes(std::uint64_t capture_bytes);

}  // namespace ps::compute
