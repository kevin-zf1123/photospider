---
spec_schema_version: 1
id: NUM-12A
parent_id: NUM-12
function: sort
proposed_operation_keys:
  - array.sort_strict
  - array.sort_accelerated_apple_silicon
  - array.sort_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-12A: sort

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Stable ascending sort of every logical line along required static Int64 axis.
Input `input` supports UInt8/Int64/Float32/Float64. Named outputs `values` and
`indices` both have input shape; values preserves dtype and indices is Int64,
containing each selected element's original coordinate on the sorted axis.
Output facets are empty. Axis is nonnegative and less than rank; no descending,
unstable, implicit cast or image-semantic mode is provided.

Numerical values sort in ascending order, including infinities. Signed zeros
compare equal. All NaNs sort after all non-NaNs and compare equal for ordering;
original input-axis order breaks every equality tie. Preserve original value bits,
including sNaNs and payloads, rather than quieting them. Use bit classification
before comparison to avoid signaling exceptions. Integer order is exact without
floating conversion. The three CPU profiles produce identical output bits.

Input rank is 1..8 with positive extents and logical element count <=2^40.
Both output shapes are inferred statically; original axis indices therefore fit
Int64. Only axis is a static numeric parameter. All output storage is dense per
requested rectangle; there is no view/dense parameter.

## Multi-output demand and invalidation

For each output request, project requested coordinates onto all non-axis axes.
For every selected line, read the entire input axis as exact Data support,
including when only one sorted position or only indices is requested. Other
lines have no Data demand. Add recognized typed-input Validation closure
separately. Empty requests read no samples. No early NaN or singleton shortcut
changes this full-line support contract.

Values and indices can be requested independently or jointly. Share one stable
ordering for a given line/input version; compute the union of required input
lines without allocating or publishing an unrequested output. Internal original
indices needed by stable sorting are scratch, not a forced public indices result.

The implementation uses the host's optional accounted pure-block cache to reuse
a completed line permutation across outputs. Reuse requires positive result
cache capacity and admitted proof work. Cache-off, eviction or proof exhaustion
may recompute the same stable order; there is no once-per-Run evaluation promise.
Each observation still obtains its complete current source line and independent
Data/Validation witness.
For values at sorted coordinate k, copy the source value at the corresponding
stable original index. No additional source outside the full line is needed.

Any source value change can alter the whole line order, so invalidate that line
in both outputs; retained typed-validation changes invalidate their observations.
Cache identity includes axis/profile, dtype/shape, original logical order and
source/validation witnesses. Correctness and output bits do not depend on sort
chunk sizes, worker order or whether the other output was requested earlier.

## Algorithms, resources and errors

A reference implementation uses stable comparison sorting or total keys
(numeric/NaN class, original index) with O(L log L) work and O(L) bounded scratch
per active line of length L. A proven partial-order selection may reduce work
for sparse sorted positions while keeping exactly the same complete stable order
and full-line support. No approximation of ranks is permitted.

Account raw values, original-index/permutation buffers, comparison scratch,
retained source owners/windows, dependency metadata and only requested output
payloads. Process bounded active lines under host work/capacity/stage limits;
large lines may fail ResourceExhausted, without silently spilling to unaccounted
disk storage or dropping stability. Poll cancellation during ingestion, sorting
passes/comparison blocks and before publication. Read legal immutable arbitrary
strides/offsets; publish packed owned fragments with correct global origins.
No writable source alias or unrequested output cells are exposed.

Compile/preflight rejects dtype/rank/shape/axis violations. Generic NaN/Inf data
sorts successfully. Runtime upstream/typed/resource/cancellation failures retain
existing categories. Failed observations publish no partial output; published
owners survive context destruction. Independent completed observations follow
the runtime's normal terminal rules. Apply the usual cache-off and owner-release
contract without forcing either complete logical output into memory.

## Acceptance and implementation status

Fixture: input=[3,1,1,2], axis=0 yields values=[1,1,2,3] and
indices=[1,2,3,0]. Independent stable ordering with original-index tie breaks
is the oracle. Float fixture [3,-0,+0,NaN_a,2,NaN_b] yields indices
[1,2,4,0,3,5], preserving both NaN bit patterns and zero order in values.

Test each output alone and joint equality, partial sorted positions, unrequested
lines with failing upstream values, all-NaN lines, singleton lines, repeated
integers above 2^53, reverse/zero input strides, multi-axis shapes, source-read
logs, full-line dirty propagation, sorting scratch limits, cancellation and
result lifetime. The current three profile keys use the public ordering workflow
and exact stable ordering implementation. The
[numeric workflow README](../../../../examples/numeric_workflow/README.md)
records the manual evidence. Proposed status is unchanged.
