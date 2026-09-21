---
spec_schema_version: 1
id: NUM-11E
parent_id: NUM-11
function: reduce_count
proposed_operation_keys:
  - numeric.reduce_count_strict
  - numeric.reduce_count_accelerated_apple_silicon
  - numeric.reduce_count_accelerated_x86_64
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

# NUM-11E: reduce_count

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Count logical elements per reduction group using only input metadata. Accept
UInt8/Int64/Float32/Float64 `input`; output `values` is Int64, with axes/shape and
fixed keepdims semantics from the [reduction contract](NUM-11_reduction_contract.md).
Only axes is a static parameter; there is no dtype or finite/nonzero-count mode.
Output facets are empty. Each result equals product(input.shape[j], j in axes),
an exact integer in [1,2^40]. All three profiles produce identical value bits.

## Metadata-only dependence and storage

All elements count, including zeros, NaNs and infinities; no element needs to be
read to establish this fact. Use Descriptor/metadata support only, with empty
Data/Control/numeric Validation support. An upstream numeric failure is not
requested simply to produce this count. Metadata/schema validation still occurs.
Changing sample bytes without changing shape does not invalidate count. Shape,
axes/profile and metadata dependencies belong to output inference/cache identity.
Empty output requests publish no values and request no upstream sample work.

Compute the checked axis product with O(rank) work. The implementation may own
one Int64 count and expose one complete immutable zero-stride output before
projection; no dense physical layout is promised. Account actual
count backing/output fragments and metadata, and do not reserve the entire
logical payload merely to express repeated counts. Keep exact Region origins and
coverage, with no implicit missing data. Final owners may outlive the context.

## Errors and acceptance

Compile/preflight rejects unsupported metadata, invalid axes and source shape
outside the common cap. No floating domain or value-validation error is produced.
Resource/cancellation errors retain existing Status semantics; poll before
publication and during any materialized chunk fill, releasing unpublished work
on failure. Do not use an input-reader helper that implicitly executes upstream
sample computation.

Fixture: input shape [2,3,4], axes="1,2" produces Int64 shape [2,1,1]
with values [[[12]],[[12]]], independently of sample bytes. An instrumented
upstream whose numeric evaluation would fail must show zero sample reads. Test
all dtypes, NaN/Inf/zero-filled source descriptors, full reduction, axis order
normalization, invalid/duplicate axes, shape changes, byte-only source changes,
partial output requests, resource cleanup and count-owner lifetime through the
public manual target. The formal keys execute Whole and preserve the numerical rules above. See
[NUM-11 Whole execution](../reductions-whole.md) for current public workflow,
validation and timing. Earlier regional platform records predate Whole.
