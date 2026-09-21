---
spec_schema_version: 1
id: NUM-08A
parent_id: NUM-08
function: mix
proposed_operation_keys:
  - numeric.mix_strict
  - numeric.mix_accelerated_apple_silicon
  - numeric.mix_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-08A: mix

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Mix matching floating elements by (1-t)*a+t*b. Dynamic inputs in order are
`a`, `b`, `t`, with identical positive rank-1..8 shapes and Float32/Float64 dtype.
Output `values` preserves dtype/shape with empty facets. There are no static
numeric parameters or implicit casts/broadcasting. A common blend factor is
explicitly broadcast to shape. This generic numeric operation does not infer
color space, alpha association or image compositing behavior.

For every logical coordinate in a nonempty request, t must be finite and satisfy 0<=t<=1. Negative
zero is valid zero. An invalid t fails with InvalidArgument, FailureReason::InvalidDomain and diagnostic tag InvalidMixFactor,
reporting coordinate and value bits. No linear extrapolation is provided.
Invalid factors outside the consumer projection also fail the invocation,
with Run scope and no Atom key. All three CPU profiles follow the common
NUM version policy.

## Numeric and Whole execution contract

Every nonempty request collects and typed-validates all three complete inputs
before the synchronous callback checks t and computes the complete packed output.
Empty reads no payload and invokes no callback. At t=0 copy a bits; at t=1 copy
b bits. These endpoint copies preserve sNaN and signed zero without quieting.
Both branches remain source and validation obligations even at endpoints.
Unselected source/typed failures are visible and may precede InvalidMixFactor.
This eager execution choice was confirmed for the 2026-09-21 Whole migration.

For 0<t<1, both values are required. Propagate a sole NaN, or a if both are NaN,
preserving sign/payload and quieting it. One infinity paired with a finite value,
or two same-sign infinities, returns that infinity. Opposite infinities yield
the fixed positive quiet NaN. These are successful numeric results.

For finite a/b in the interior, correctly round the exact real expression
(1-t)*a+t*b directly to dtype once. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound. Two
negative-zero endpoints yield -0; other exact zero interior results yield +0.
A nonzero exact result rounding to zero retains its mathematical sign. Since
t is in [0,1], finite endpoints cannot produce an infinite mathematical-range
result. Avoid naive subtraction or products that create spurious overflow.

Use exact dyadic arithmetic or a certified equivalent. Any change to any input,
including an unselected endpoint, invalidates all observed outputs. Inherit
[select's Whole input, owner and resource requirements](NUM-07H_select.md).
The executor projects the complete output to the consumer's global coordinates.
## Resource, error and acceptance requirements

Work is O(full logical elements) plus exact arithmetic. Capacity includes three
full input collections, full N × dtype-width output and one fixed interpolation
workspace. Sparse requests may consume substantially more memory/work. Publish
no partial output on failure. Check cancellation each element, inside exact
arithmetic and before publication. Release unpublished storage on all failures;
owned results survive their context. Per-value numerical diagnostics are N/A.
The public `examples/numeric_workflow/interpolation.cpp` fixture uses
`WorkflowDocument`, `Compiler`, `ExecutionContext` and explicit broadcasts
of endpoint and edge scalars. It checks `a=[10,10,10]`, `b=[20,20,20]`,
`t=[0,0.25,1] -> [10,12.5,20]`, complete input support, eager branch failure,
cache invalidation, layouts, Empty, caller fenv and resource/cancellation cleanup.
The current local strict/Apple Whole runs pass the 5,244-case independent
Fraction/IEEE oracle per profile. Historical WSL/installed-consumer passes were
for the pre-Whole implementation and are not current execution evidence.
See [NUM-08 Whole measurements](../interpolation-whole.md). The manual target
has no CTest/integration registration; specification status remains Proposed.
