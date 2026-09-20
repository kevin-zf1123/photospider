---
spec_schema_version: 1
id: NUM-07H
parent_id: NUM-07
function: select
proposed_operation_keys:
  - numeric.select_strict
  - numeric.select_accelerated_apple_silicon
  - numeric.select_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-07H: select

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Select an element from one of two arrays using a boolean-valued UInt8 condition.
Inputs in order are `condition`, `when_true`, `when_false`; all have the same
rank-1..8 positive shape. Both value branches share one dtype chosen from UInt8,
Int64, Float32 and Float64. Output `values` uses the branch dtype, unchanged
shape and empty facets. Infer dtype from when_true, not the UInt8 condition;
the registry's output_dtype_input field can identify this source input.
There are no static numeric parameters or implicit broadcasts/casts.

At every logical position in a nonempty request, condition must be exactly 0 or 1. Zero selects
when_false; one selects when_true. Other UInt8 values fail with InvalidArgument,
FailureReason::InvalidDomain and diagnostic tag InvalidCondition, global linear index and actual byte. Output copies the
selected element's exact bits, including NaN payload, signaling NaN, infinity
and signed zero. This is copying: it does not apply arithmetic NaN quieting.
All three CPU versions are bitwise equivalent; platform-specific keys require
their named CPU platform.

## Whole execution and invalidation

Both branches' dtype/shape metadata is checked at compile/preflight. Every
nonempty request collects and validates condition and both complete branches
before entering a synchronous callback. The callback verifies each condition
byte and copies the chosen value bits into a complete packed owned output.
The executor projects it to requested global coordinates. Empty reads no payload
and invokes no callback.

An unselected branch source or typed failure affects the invocation. Input
collection and typed validation may fail before the callback can diagnose an
invalid condition. After successful input collection, an invalid byte anywhere,
including outside the consumer projection, fails with InvalidArgument/InvalidDomain,
Run scope and no Atom key. No partial output is published. This eager input
behavior was explicitly selected for the Whole migration on 2026-09-21.

Any edit to condition or either branch invalidates all observed outputs, including
edits at unselected locations. Numerical bit selection remains unchanged; source
NaNs, signaling NaNs and signed zeros use raw integer/byte transport.

## Storage and resources

Read legal immutable strided/offset/unaligned arrays, including zero/negative
strides and shifted origins. Capacity includes the full UInt8 condition, both
full branch collections and N × branch-width output. Fixed scalar locals replace
condition-dependent set/continuation state. Sparse requests may cost more memory
and work. Work is O(N), cancellation is checked before each element and final
publication, and unpublished output is released on all failures. Published owners
remain valid after context destruction. Per-value numeric diagnostics are N/A.

## Acceptance and implementation status

Conceptual fixture: condition=[1,0,1], when_true=[10,20,30],
when_false=[1,2,3] -> [10,2,30]. All three source supports cover {0,1,2}.
Check that unselected source/typed failures are visible, collection failure
precedes invalid-condition evaluation, byte 2 outside a consumer projection
fails the invocation, and edits to either branch invalidate cache reuse.
Cover all-true/all-false/alternating conditions, all four branch dtypes, distinct
sNaN payloads, strides, output budgets, cancellation and escaped output owners.

The public manual workflow verifies these Whole rules locally for strict/Apple.
The earlier 2026-09-14 WSL/installed-consumer checks described the pre-Whole
staged implementation and do not verify this migration. Current commands and
public/core measurements are in [NUM-07 measurements](../comparison-whole.md).
No integration registration is added; specification status remains Proposed.
