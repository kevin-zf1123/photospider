#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
/** @brief Authors a bounded named-coefficient expression sampler.
 * @param id Nonzero node id.
 * @param expression Nonempty ASCII mathematical source, <=4096 bytes, <=256
 * AST nodes and height <=32. See NUM-01 for grammar and evaluation order.
 * @param start Float32/64 [1] endpoint connection.
 * @param end Float32/64 [1] endpoint connection; statically present but unread
 * for count=1. Equal endpoints are invalid at runtime for count>=2.
 * @param count Number of samples, in [1,1048576].
 * @param coefficients Exact free-name/connection map. Names are validated,
 * bytewise sorted and serialized explicitly; unused/missing names reject.
 * @param dtype Float32 or Float64 output, default Float64 explicitly authored.
 * @param profile Explicit CPU implementation; unsupported targets fail at
 * compile/preflight. The default is Strict.
 * @return Owned node with values:[count] and axis:Float64[3]. Invalid source,
 * names/id/count/dtype/profile fails InvalidArgument/InvalidDomain/Schema.
 * Allocation can throw bad_alloc; the helper is pure/concurrent-safe.
 * @note Compilation owns one immutable AST across requests and dynamic runs.
 * Inputs may have mixed floating dtypes. Runtime computes RN64 coordinates and
 * checks requested adjacent-coordinate separation. Strict evaluates
 * left-to-right postorder with RN64 at each primitive and one final dtype
 * conversion. Accelerated retains that stepwise result as its reference and
 * certifies the final output under CpuNumericProfile's FP32 bound, replaying
 * uncertain samples strictly. Whole, ROI and SIMD tails retain the same
 * fixed-profile results. All required inputs/intermediates must be finite;
 * numeric failures identify the requested Atom and source span.
 * Work/capacity/cancellation/upstream failures retain their categories. Bounded
 * strict refinement can exhaust. Both outputs own immutable packed bytes beyond
 * context lifetime. Axis-only skips coefficients/AST; Empty skips all payloads.
 * Cache witnesses retain evaluated and validation dependencies even under
 * algebraic cancellation. Strict math calls and per-function accelerated
 * fallbacks are diagnostics.
 */
PHOTOSPIDER_API Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients = {},
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict);
}  // namespace ps::numeric
