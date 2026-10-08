#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
/** @brief Authors a Whole Result sampler for a bounded expression.
 * @param id Nonzero node id.
 * @param expression Nonempty ASCII mathematical source, <=4096 bytes, <=256
 * AST nodes and height <=32. See NUM-01 for grammar and evaluation order.
 * @param start Workflow input bound to a one-element Float32/Float64 tensor
 * Result.
 * @param end Workflow input bound to a one-element Float32/Float64 tensor
 * Result. Its schema is checked at compile time, but its sample and producer
 * are unused for count=1. Equal endpoints fail at runtime for count>=2.
 * @param count Number of samples, in [1,1048576].
 * @param coefficients Exact free-name/connection map. Names are validated,
 * bytewise sorted and serialized explicitly; unused or missing names reject.
 * Each connection is a one-element Float32/Float64 tensor Result; dtypes may
 * mix.
 * @param dtype Float32 or Float64 output, default Float64 explicitly authored.
 * @param profile Explicit CPU implementation; unsupported targets fail at
 * compile/preflight. The default is Strict.
 * @return An owned node with two independent Result outputs: `values` is
 * `photospider.tensor` v1 / `samples`, shape [count], in the selected dtype;
 * `axis` is the same schema/member, Float64 shape [3], with one atomic trailing
 * tuple [start,end,step] (or [start,start,+0] for count=1). Output tensors use
 * ordinary sample axes and inherit neither input batch axes nor facets. Invalid
 * source,
 * names, id, count, dtype or profile returns
 * InvalidArgument/InvalidDomain/Schema. Allocation can throw std::bad_alloc.
 * The helper is pure and safe for concurrent use.
 * @note Each input Result has exactly one tensor member under any key; schema
 * IDs may differ and fields may coexist. Compilation validates every declared
 * input schema, member shape and dtype, then shares one immutable AST across
 * both output programs and dynamic runs. Runtime computes RN64 coordinates. A
 * values request checks adjacent coordinate separation over the full grid and
 * evaluates all count samples before projection. Strict evaluates each
 * primitive left-to-right in postorder with RN64 and converts the final value
 * once. Accelerated evaluation
 * certifies the strict stepwise result under CpuNumericProfile's FP32 bound
 * and replays uncertain samples strictly. All active inputs and intermediates
 * must be finite. A numeric failure identifies global sample, x and source
 * span with Domain/Run scope; WorkLimit, capacity, cancellation, upstream and
 * stale failures retain their categories. Directed refinement may return
 * ResourceExhausted. Arithmetic restores the worker floating environment, and
 * the invocation preserves the caller's rounding mode and exception flags.
 *
 * A nonempty Whole request needs active inputs with Data, Validation and
 * Descriptor roles. The coordinator supplies authorized tensor windows, so
 * legal input strides do not require packed input collection. `values` writes
 * the complete packed array before requested coordinates are projected. `axis`
 * requests only active endpoints and publishes its independent three-component
 * tuple; it never evaluates the expression or reads coefficients. For count=1,
 * both outputs exclude `end` at runtime, while values still uses every named
 * coefficient. Empty queries produce empty coverage without sample reads or
 * arithmetic. Changes to active inputs invalidate their dependent output;
 * coefficients never dirty axis. Cache witnesses retain exact active input
 * dependencies even when algebraic cancellation removes a coefficient from
 * the numeric result. Each output has its own ObjectId and its association
 * records only its active inputs; there is no extra identity joining values
 * and axis. Whole timing reports poll count and computed elements; per-value
 * arithmetic and fallback counters are unavailable for this path.
 *
 * Values owns count*sizeof(dtype) output bytes plus bounded math workspace;
 * axis owns 24 output bytes and smaller coordinate workspace. Both Results own
 * immutable storage beyond context lifetime. See the NUM-01 specification and
 * examples/numeric_workflow for the grammar, error and resource rules.
 */
PHOTOSPIDER_API Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients = {},
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict);
}  // namespace ps::numeric
