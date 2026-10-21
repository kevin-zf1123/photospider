#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include "photospider/compiler/compiler.hpp"
#include "photospider/ops/export.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"
#include "photospider/ops/numeric/lut3d.hpp"
#include "photospider/ops/numeric/lut3d_bake.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

namespace ps::numeric {
/** @brief Generated source input, always a Float64 Result tensor with final
 * extent-3 component axis. Grid shape is [N0,N1,N2,3], validation shape is
 * [P,3]. Source code must be independently pointwise and handle these complete
 * sample shapes without reading other sampled colors. Shared dynamic inputs use
 * ordinary existing graph edges.
 */
struct Lut3dSourceInput {
  WorkflowNodeOutput colors;
  TensorDescriptor descriptor;
};
/** @brief Authoring-only source expansion, called twice on a staged document.
 * Append ordinary nodes and return an output that resolves to one Result tensor
 * with the supplied complete sample shape and Float32/64 dtype. The builder
 * supplies source interpretation explicitly through node parameters. Preserve
 * existing input declarations (including Result schemas), nodes and exports. No
 * computation, runtime callback or captured closure is retained. The supplied
 * graph's keys/profiles and explicit casts define the reference.
 */
using Lut3dSourceBuilder = std::function<Result<WorkflowNodeOutput>(
    WorkflowDocument&, const Lut3dSourceInput&)>;
/** @brief Required measured-bake choices. Construction requires explicit shape,
 * interpolation, tolerances, descriptions and pointwise assertion. table_dtype
 * defaults to the inferred source output dtype; profile selects only generated
 * sampling/application facilities, never replaces source operation keys.
 */
struct Lut3dBakeOptions {
  std::array<std::uint64_t, 3> shape{};
  Lut3dInterpolation interpolation = static_cast<Lut3dInterpolation>(0);
  double atol = -1, rtol = -1;
  TensorDescription input_description, output_description;
  bool source_pointwise = false;
  std::optional<ElementType> table_dtype;
  CpuNumericProfile profile = CpuNumericProfile::Strict;
};
struct Lut3dValidationPoints {
  WorkflowInput values;
  /** @brief Authoring hint 1..1048576; Compiler checks one Float64 Result
   * tensor with full `sample_shape()` [count,3].
   */
  std::uint64_t count = 0;
};
/** @brief Connectable exports; table is a globally quality-gated
 * `photospider.tensor` v1 Result with `samples` shape [N0,N1,N2,3], selected
 * dtype and atomic trailing color axis. Axis is an independent Float64 tensor.
 * report uses curve.bake_lut3d.report v4. Read a completed Result with the
 * public read_lut3d_bake_report helper and description. Its measured_table_id
 * tensor identifies the complete table object acquired during measurement. This
 * identity is local to the producing Run; names are not auto-exported.
 */
struct BakedLut3d {
  WorkflowNodeOutput table, axis, report;
  /** @brief Owned static configuration for explicit report reading and reuse.
   * It is not attached to any generated Result.
   */
  Lut3dBakeDescription description;
};
/** @brief Appends an ordinary graph for measured source sampling and LUT
 * baking. registry must be frozen and include the source keys. Compiler
 * metadata checks both source expansions, ports/descriptions and records their
 * semantic digest. axis is dynamic Float64 [3,3]; optional extra points are
 * Float64 [M,3]. All samples and references execute in the final graph's one
 * immutable binding snapshot, including shared inputs. No nested execution or
 * file save occurs.
 *
 * Geometry keys use fixed CPU tasks for selected output tiles. Axis copies
 * selected cells; grid/centers read six endpoints and use fixed ExactSampling
 * scratch. Extra-only tiles read selected extra rows with zero scratch. Color
 * copies or converts corresponding complete colors once, with zero scratch.
 * Finite consistent axes, model-valid colors, in-range extras and finite
 * converted values are caller preconditions. Inputs retain their own tile
 * grids; outputs use ordinary axes plus one atomic component axis except for
 * axis. Empty emits no task. Concurrent Runs own independent scratch and
 * outputs.
 *
 * Pack/unpack/gate use one fixed CPU task per selected output tile and copy
 * raw samples into independent dense storage. Pack/unpack require no scratch.
 * A nonempty Measure emits one task that waits for the complete owned table,
 * reads all validation points/reference/applied colors and writes all thirteen
 * report members using fixed exact-integer scratch. Projecting the report or
 * gated table preserves this global measurement closure. Its retired CallRecord
 * exports the report, so an observer can retain it when a later Gate fails.
 *
 * Report v4 records the actual owned-table ObjectId through the invocation's
 * input association. Gate first checks the configuration identity
 * (TypeMismatch) and table association (OperationFailed/InvalidAssociation),
 * then the measured tolerance result. A legitimate exceeded tolerance
 * successfully produces a passed=false report; Gate fails
 * OperationFailed/InvalidDomain with LutApproximationToleranceExceeded and
 * first-failure details. Valid finite colors, consistent axes and well-formed
 * report values are caller premises; the SDK report reader independently
 * validates its input. Measure and Gate are nondeterministic: fresh owned
 * tables receive fresh ObjectIds per Run. Axis-only requests read neither
 * source nor extra points. Cancellation and resource failures propagate. Result
 * owners survive context teardown. Acceptance applies only at the listed points
 * using the selected interpolation and table dtype; it provides no
 * continuous-domain bound.
 *
 * Calls modifying one document must be serialized. All authoring is staged;
 * failure or exception leaves document unchanged. Allocation or a source
 * builder may throw; callback side effects outside the staged document are the
 * caller's responsibility. The builder and captures are released when this call
 * returns.
 */
PHOTOSPIDER_OPS_API Result<BakedLut3d> bake_lut3d(
    WorkflowDocument& document, std::shared_ptr<OperationRegistry> registry,
    WorkflowInput axis, const Lut3dSourceBuilder& source,
    const Lut3dBakeOptions& options,
    std::optional<Lut3dValidationPoints> validation_points = {},
    ResourceSet resources = {});
}  // namespace ps::numeric
