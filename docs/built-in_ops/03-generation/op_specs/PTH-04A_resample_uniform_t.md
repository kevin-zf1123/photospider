---
spec_schema_version: 1
id: PTH-04A
parent_id: PTH-04
function: resample_uniform_t
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.resample_uniform_t_v1_strict
---

# PTH-04A: resample uniform t

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet; PathSamplesParam Result with positions,source_segment,local_t.

The associated Result uses `photospider.ops.path_samples_param` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

samples_per_segment n>=2, hard max_count; endpoint_mode=per_segment.

## Mathematical specialization

For every evaluable segment use exact t_j=j/(n-1); RN64 positions from the complete
formula, local_t rounded independently. Order subpath/segment/j; Z is a segment. Shared
endpoints may repeat. M-only emits its point once; FullTurn includes duplicate closure
endpoint.

## Runtime implementation

`path_edit_basic.cpp` implements the strict key for CoreVerbs and Primitives. `samples_per_segment` defaults to 2; `max_count` is a static capacity. Evaluation uses the exact rational sample parameter, with independently rounded positions and `local_t`. `generation::path_samples_schema` constructs the public parameter-sample schema.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input Result; checked static capacity arithmetic and unchecked payload counts,
no arc-length computation.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Unequal distances at uniform t, shared endpoint duplication,n2 endpoints and empty
Result.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
