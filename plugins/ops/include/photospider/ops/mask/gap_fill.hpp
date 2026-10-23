#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/export.hpp"

namespace ps::mask {
/** @brief Named MASK-16B/C graph edges, owned by the expanded document.
 *
 * Each edge produces Binary[H,W] in the seeds Float32/Float64 dtype. Barrier
 * and seeds may have different floating dtypes but must have identical
 * spatial shapes. Export any subset using these names in document.outputs.
 * Diagnostic-only selections do not execute the flood node. Any selected
 * output has complete-input support for the original barrier and seeds.
 * Results own their storage independently of input and context lifetime.
 */
struct GapFillOutputs final {
  WorkflowNodeOutput fill, barrier, ignored_seeds, reopened_barrier;
};
/** @brief Explicit MASK-05 footprint encoding used by MASK-16C.
 *
 * kind is square, diamond, disk, or custom. Analytic footprints require
 * radius only. Custom footprints require offsets only, encoded as canonical
 * sorted unique "dy:dx;..." pairs including the origin and their negatives.
 * Radius and offsets define index-grid support, independent of physical
 * distance units. Static support arithmetic must be representable.
 */
struct GapFillFootprint final {
  std::string kind;
  std::optional<std::int64_t> radius;
  std::optional<std::string> offsets;
};
/** @brief Append the MASK-16B axis-gap closure, seed override and flood.
 *
 * The three nodes use bridge_axis_gaps_strict, prepare_gap_fill_strict and
 * flood_barrier_strict. There is no mask.fill_axis_gaps registry entry.
 * Seeds on original barriers are ignored; surviving seeds reopen exactly
 * their own newly closed cells. Source Results are immutable. All samples
 * satisfy the unchecked Binary premise; no payload validation is added.
 * @param document Caller-owned graph, changed transactionally on success.
 * Serialize edits to the same document; independent documents are thread-safe.
 * @param barrier Original Binary[H,W] edge.
 * @param seeds Binary[H,W] seed edge.
 * @param maximum_gap Nonnegative maximum number of missing cells in an axis
 * run.
 * @param fill_connectivity Exactly 4 or 8.
 * @param max_work Optional nonnegative operational guard per invocation.
 * @return Named graph edges, or InvalidArgument/InvalidDomain for malformed
 * options/IDs. Input schemas are checked by compilation. Allocation may throw
 * std::bad_alloc. Execution propagates resource/cancellation failures under
 * ordinary call retirement; already retired intermediate calls remain valid.
 */
PHOTOSPIDER_OPS_API Result<GapFillOutputs> fill_axis_gaps(
    WorkflowDocument& document, WorkflowInput barrier, WorkflowInput seeds,
    std::int64_t maximum_gap, std::int64_t fill_connectivity,
    std::int64_t max_work = INT64_MAX);
/** @brief Append the MASK-16C full-lattice closing, seed override and flood.
 *
 * The closing primitive preserves its extended intermediate before final
 * cropping. Preparation explicitly ORs closing with the original barrier.
 * There is no mask.fill_morphological_gaps registry entry or universal
 * maximum-gap guarantee. Named outputs, dtype, ownership, errors and thread
 * rules match fill_axis_gaps.
 * @param document Caller-owned graph, changed transactionally on success.
 * @param barrier Original Binary[H,W] edge.
 * @param seeds Binary[H,W] seed edge.
 * @param footprint Explicit MASK-05 footprint.
 * @param fill_connectivity Exactly 4 or 8.
 * @param max_work Nonnegative per-invocation operational guard.
 * @return Named graph edges or the original structural validation failure.
 */
PHOTOSPIDER_OPS_API Result<GapFillOutputs> fill_morphological_gaps(
    WorkflowDocument& document, WorkflowInput barrier, WorkflowInput seeds,
    const GapFillFootprint& footprint, std::int64_t fill_connectivity,
    std::int64_t max_work = INT64_MAX);
}  // namespace ps::mask
