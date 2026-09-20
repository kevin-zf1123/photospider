#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include "photospider/compiler/compiler.hpp"
#include "photospider/data/lut3d_bake.hpp"
#include "photospider/numeric/lut3d.hpp"
#include "photospider/numeric/workflow_authoring.hpp"

namespace ps::numeric {
/** @brief Generated source input, always Float64 with final component axis 3.
 * Grid shape is [N0,N1,N2,3], validation shape is [P,3]. Source code must be
 * independently pointwise and handle these batch shapes without reading other
 * sampled colors. Shared dynamic inputs use ordinary existing graph edges.
 */
struct Lut3dSourceInput {
  WorkflowNodeOutput colors;
  ValueDescriptor descriptor;
};
/** @brief Authoring-only source expansion, called twice on a staged document.
 * Append ordinary nodes and return their output; preserve existing
 * declarations, nodes and exports. No computation, runtime callback or captured
 * closure is retained. The supplied graph's keys/profiles and explicit casts
 * define the reference. Float32/64 output must preserve the supplied batch
 * shape.
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
  ColorArrayDescriptor input_description, output_description;
  bool source_pointwise = false;
  std::optional<ElementType> table_dtype;
  CpuNumericProfile profile = CpuNumericProfile::Strict;
};
struct Lut3dValidationPoints {
  WorkflowInput values;
  /** @brief Authoring hint 1..1048576; Compiler checks Float64 [count,3]. */
  std::uint64_t count = 0;
};
/** @brief Connectable exports; table is globally quality-gated, axis
 * independent. report is a curve.bake_lut3d.report v1 ResultRef; read it with
 * the public read_lut3d_bake_report helper after execution. Names are not
 * auto-exported.
 */
struct BakedLut3d {
  WorkflowNodeOutput table, axis, report;
};
/** @brief Appends an ordinary graph for measured source sampling and LUT
 * baking. registry must be frozen and include the source keys. Compiler
 * metadata checks both source expansions, ports/descriptions and records their
 * semantic digest. axis is dynamic Float64 [3,3]; optional extra points are
 * Float64 [M,3]. All samples and references execute in the final graph's one
 * immutable binding snapshot, including shared inputs. No nested execution or
 * file save occurs.
 *
 * Every nonempty table or report request validates all grid colors, complete
 * source outputs, centers and extra points. report successfully records
 * passed=false on exceeded tolerances; table then fails OperationFailed/
 * InvalidDomain with LutApproximationToleranceExceeded. Axis-only observes no
 * source or extra points. Source/shape/domain/work/capacity/cancellation/stale
 * failures retain their categories. Result and packed Value owners survive
 * context teardown. Measured acceptance applies only at listed points using
 * the chosen interpolation and table output dtype, never the continuous domain.
 *
 * Calls modifying one document must be serialized. All authoring is staged;
 * failure or exception leaves document unchanged. Allocation or a source
 * builder may throw; callback side effects outside the staged document are the
 * caller's responsibility. The builder and captures are released when this call
 * returns.
 */
PHOTOSPIDER_API Result<BakedLut3d> bake_lut3d(
    WorkflowDocument& document, std::shared_ptr<OperationRegistry> registry,
    WorkflowInput axis, const Lut3dSourceBuilder& source,
    const Lut3dBakeOptions& options,
    std::optional<Lut3dValidationPoints> validation_points = {},
    ResourceBindings resources = {});
}  // namespace ps::numeric
