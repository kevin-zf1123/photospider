#pragma once

#include "photospider/data/layer.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
enum class LayerOperation : std::uint32_t {
  Assemble = 1,
  Over,
  Opacity,
  EmitFront,
  EmitBehind,
  Flatten,
  Response,
  ResponseOver,
  CoverageRawPlus,
  RawChecked,
  RawCapped,
  Weight,
  Reduce,
  Finalize,
  RequireValid
};
/** @brief Creates one ready-to-register CPU operation for a fixed raster/space.
 * The returned canonical layer.* key identifies its arithmetic; register once
 * per registry. The caller may give a distinct key for another fixed raster.
 * Assembly consumes canonical RGBA Float32 HWC4 and explicitly interprets a
 * generic, facet-free Float32 HWC3 as independent emission in the named space.
 * Emit/Flatten use that same explicit RGB input for emission/opaque background.
 * Opacity and Emit require a Float64 "factor" (rounded to binary32); Weight
 * requires a nonnegative Float64 "weight" and emits raster-ordinal
 * contributions. CoverageRawPlus explicitly selects only coverage from its two
 * Layer inputs. RawChecked/RawCapped produce zero-emission Layers with explicit
 * mass policy. Reduce accepts a dynamic ordered collection, seals count before
 * arithmetic, then uses [lo,hi) -> [lo,lo+(hi-lo)/2), [mid,hi) for all eight
 * binary64 sums. A host-owned depth-64 stack and bounded pages make grouping
 * independent of physical windows. Empty input emits a zero sum. Finalize emits
 * OptionalLayer; RequireValid rejects valid=false before image conversion.
 * Outputs are complete associated results except Flatten, an ordinary RGBA
 * Value sink requiring a Whole request and enough budget for its dense output.
 * Every operation uses the managed root, mandatory backing, charged finite work
 * and Conservative(All) support including empty-result descriptor support.
 * No GPU or certified numerical-error guarantee is declared. No callback I/O.
 */
PHOTOSPIDER_API Result<OperationDefinition> make_layer_operation(
    LayerOperation operation, const LayerSpec& spec = {});
}  // namespace ps
