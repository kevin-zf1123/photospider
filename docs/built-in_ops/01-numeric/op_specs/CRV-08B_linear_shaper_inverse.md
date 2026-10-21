---
spec_schema_version: 1
id: CRV-08B
parent_id: CRV-08
function: linear_shaper_inverse
proposed_template_names:
  - curve.linear_shaper_inverse
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08B: linear_shaper_inverse

The public `linear_shaper_inverse` helper expands ordinary workflow nodes; it does not register a new primitive. Ordered inputs are single-tensor Results input, lower and upper, with arbitrary schema/member names. Complete sample shapes include batch axes. Input is Float32/Float64, rank 1..8, positive extents and at most 2^40 elements. Bounds are same-dtype scalars with shape `[1]`. Output `values` uses `photospider.tensor`/`samples` with ordinary input axes and unchanged dtype.

The mathematical formula is `lower+input*(upper-lower)`, with one final rounding of the complete remap expression and no clipping. Finite ordered bounds `lower<upper` are an unchecked caller precondition. Within that domain, endpoint bits, NaN quieting/payload, signed zero and infinity extensions follow [remap_range](NUM-06B_remap_range.md). Strict results are bitwise reproducible; accelerated profiles retain the [shared floating bound](NUM_accelerated_contract.md). Invalid bound payloads have no promised runtime diagnostic.

## Fixed composition and execution

The helper constructs scalar zero/one literals, expands bounds and literals through fixed constant tasks, and applies fixed remap tasks. Each selected output tile and selected intermediate has an independent dense allocation. Empty selection emits no payload work. Source affine views may have negative/zero strides and unaligned offsets after bounds validation. All admitted intermediates count toward block capacity, and successful output owners outlive the execution context.

The inverse retains a scalar `remap(lower,lower,upper,lower,lower)` dependency before target-lower expansion. This preserves lower bits for valid bounds; it is not a runtime order validator. Shape/dtype mismatches fail before execution. Resource, backend, cancellation and upstream failures retain their operational statuses. Fixed tasks use the declared sample dependencies and preallocated output tiles, with no complete-input validation, content cache or full-output publication callback.

Both Float32 and Float64 compositions use fixed operations. Float32 zero/one literals use count-one linspace with direct Float32 rounding; the unused endpoint and axis output add no payload tasks. Host profile availability remains required.

## Acceptance

For a valid fixture, lower=-2, upper=2, input=[0,0.5,1,1.5] gives [-2,0,2,4]. Temporary public Compiler/ExecutionContext checks compare an independent exact rational formula, including endpoints, IEEE specials, partial/empty selection, source layouts, concurrent bindings and output ownership. Check only supported execution paths and state any remaining dependency limitation. Out-of-domain bounds are not rejection tests for the unchecked fixed kernel. See the [CRV-08 linear composition](CRV-08_shaper.md#linear-composition), [fixed arrays](../arrays-execution.md) and [fixed ranges](../range-execution.md). Specification status remains Proposed.
