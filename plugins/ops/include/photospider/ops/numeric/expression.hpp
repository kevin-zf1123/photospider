#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/ops/export.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
/** @brief Authors fixed CPU sampling of a bounded scalar expression.
 * @param id Nonzero node id.
 * @param expression ASCII source, 1..4096 bytes, at most 256 AST nodes and
 * height 32. NUM-01 defines grammar and left-to-right postorder evaluation.
 * @param start One-member Float32/Float64 Result with sample shape [1].
 * @param end Same scalar contract; dtype may differ. Its schema is always
 * checked; count one omits its payload and producer. Larger counts require
 * distinct endpoints as a caller numerical precondition.
 * @param count Number of samples in [1,1048576].
 * @param coefficients Exact free-name/connection map. Names are bytewise sorted
 * and serialized; missing/unused names reject. Each input is a Float32/64 [1]
 * scalar Result under any schema/member key; dtypes may mix.
 * @param dtype Values Float32 or Float64, default Float64 explicitly authored.
 * @param profile Explicit CPU key; unavailable platforms fail preparation.
 * @return Owned node with values: ordinary [count] in dtype, and axis: atomic
 * Float64 [3], both photospider.tensor v1/member samples. Axis contains
 * [start,end,RN64((end-start)/(count-1))], or [start,start,+0] for count one.
 * Invalid source, names, id, count, dtype or profile returns
 * InvalidArgument/InvalidDomain/Schema; incompatible schemas return
 * TypeMismatch during compilation. Allocation can throw bad_alloc. Authoring is
 * pure and concurrent-safe; it reads no payload and executes no expression.
 * @note Static preparation parses once and owns an immutable AST. Each selected
 * values tile has one fixed task reading active endpoints and every
 * coefficient; axis has a separate task reading endpoints only. Excluded inputs
 * retain schema references. Empty selection emits no task or payload read.
 * Runtime uses global indices and endpoint-weighted RN64 coordinates. Strict
 * rounds each primitive to Float64 before its parent; final Float32 conversion
 * is a separate rounding. Accelerated four-lane batches enclose the strict AST
 * and replay uncertain samples strictly under the final FP32-scaled error
 * bound. Active inputs, evaluated intermediates and computed outputs must be
 * finite and within their mathematical domains as unchecked caller
 * preconditions. Selected values coordinates must differ from their valid
 * adjacent coordinates; selected axis requires finite nonzero step when
 * count>1. Runtime performs no neighbor scan or numerical validation;
 * out-of-domain diagnostics are not guaranteed. Unselected expression samples
 * are not evaluated. Prepare checks both schemas, canonical write, exact active
 * scalar reads and scratch. Every invocation borrows preallocated private
 * arithmetic state and an independent dense output tile; axis uses smaller
 * state and 24 output bytes. Concurrent Runs share only immutable preparation.
 * Cancellation is checked per scalar, coordinate/batch, AST node and refinement
 * step. Bounded unresolved refinement may return ResourceExhausted. Operational
 * failures prevent the IR call's successful publication across selected
 * outputs. Successful owners can outlive ExecutionContext. Invocations restore
 * floating modes and flags.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients = {},
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict);
}  // namespace ps::numeric
