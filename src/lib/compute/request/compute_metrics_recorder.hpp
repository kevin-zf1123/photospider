#pragma once

#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Finalizes spatial inheritance and execution diagnostics for outputs.
 *
 * @throws Nothing from construction; the class owns no state.
 * @note The recorder operates after an operation returns and never changes
 *       graph topology, input ordering, or cache ownership.
 */
class ComputeMetricsRecorder {
 public:
  /**
   * @brief Completes one output's spatial and debug metadata.
   *
   * @param output Mutable completed operation output.
   * @param inputs Destination-indexed upstream outputs; disconnected slots are
   *        represented by null pointers and are skipped for inheritance.
   * @param enable_timing Whether to inspect pixel values for min/max/NaN data.
   * @param execution_ms Measured execution duration; negative values clamp to
   *        zero before integer-millisecond rounding.
   * @return Nothing.
   * @throws std::invalid_argument if immutable image endpoints/spans cannot
   * enter SpatialContext or enabled inspection receives unsupported element
   * facts.
   * @throws std::overflow_error when immutable image-window arithmetic is
   * unrepresentable.
   * @throws ReadyFenceAccessError, BufferAccessError, or std::out_of_range if
   * checked Value inspection cannot access a logical sample.
   * @throws std::bad_alloc if diagnostic device-label storage cannot allocate.
   * @note A default output spatial context inherits from the first live input,
   *       resets its local inverse transform, and completes an empty absolute
   *       ROI from output bounds. A fallback conversion failure leaves the
   *       original space, debug metadata, and named Values unchanged.
   *       Timestamp, worker id, duration, and device are recorded regardless
   *       of `enable_timing` after spatial completion succeeds. Device
   *       metadata for the first non-image named Value uses
   *       representation-neutral indexed StorageBinding inspection, so
   *       ProviderDefined and non-Ready DenseTensor Values require no payload
   *       access. Pixel statistics are inspected only for Ready host-visible
   *       named image Values. Inspection uses ImageView logical coordinates
   *       and excludes padding. Compatibility staging is never read. An
   *       all-NaN active payload retains the legacy positive/negative infinity
   *       empty-range sentinels; opaque/non-Ready resources retain
   *       callback-provided values.
   */
  static void finalize_output_metadata(
      NodeOutput& output, const std::vector<const NodeOutput*>& inputs,
      bool enable_timing, double execution_ms);
};

}  // namespace ps::compute
