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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-12A: sort

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
Int64. Only axis is a static numeric parameter. Each selected output owns complete dense storage; there is no view/dense parameter.

## Multi-output demand and invalidation

Any nonempty demand reads and validates the complete source, including all lines
and indices-only requests. Every source edit invalidates all recorded output
observations; source/typed/resource failures affect the Run. Empty reads nothing.
Each selected public output executes an independent Whole callback and allocates
only that complete output. Both callbacks may sort the same lines independently.
A single callback builds one permutation per line and reuses it for all positions.
No cross-output or changed-q permutation block cache is promised. Public keys,
output identities, stable ties and raw-value rules are unchanged.

## Algorithms, resources and errors

Iterative heapsort sorts (numerical key, original axis index). Keys are classified
once per line and comparisons reuse them. For length L, permutation and keys
require16*L element bytes plus ResourceAllocator headers/alignment/Entries,
charged as metadata; keys die after sorting and permutation after outputting the
line. Fixed comparison/exact state is admitted as payload workspace. The old
16-times-input-payload workspace bound and staged publication are removed.
Dense output and full source collection are additional allocations. Work is
O(total_input*log L), with checked reads/ordering and bounded cancellation.
No rounded rank, disk spill or private cache is introduced.

Malformed dtype/rank/shape/axis remains a schema failure. Generic NaN/Inf sorts
successfully, arbitrary legal input strides are supported, and output preserves
raw bits. Failed Whole attempts release unpublished output, state and metadata;
returned output survives context destruction. Sparse requests require the same
complete selected output storage and may regress in time/memory.

## Acceptance and implementation status

Fixture: input=[3,1,1,2], axis=0 yields values=[1,1,2,3] and
indices=[1,2,3,0]. Independent stable ordering with original-index tie breaks
is the oracle. Float fixture [3,-0,+0,NaN_a,2,NaN_b] yields indices
[1,2,4,0,3,5], preserving both NaN bit patterns and zero order in values.

Test each output alone and joint equality, partial sorted positions, unrequested
lines with failing upstream values, all-NaN lines, singleton lines, repeated
integers above 2^53, reverse/zero input strides, multi-axis shapes, source-read
logs, full-line dirty propagation, sorting scratch limits, cancellation and
result lifetime. The formal keys use Whole. Current public workflow, independent
oracle and separate public/core performance evidence are in
[NUM-12 Whole execution](../ordering-whole.md). Older regional WSL/installed
records predate Whole and do not establish current platform acceptance.
