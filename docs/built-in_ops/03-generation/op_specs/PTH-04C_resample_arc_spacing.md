---
spec_schema_version: 1
id: PTH-04C
parent_id: PTH-04
function: resample_arc_spacing
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.resample_arc_spacing_v1_strict
---

# PTH-04C: resample arc spacing

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,positive spacing[1] in px; PathSamplesArc Result.

The associated Result uses `photospider.ops.path_samples_arc` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

include_open_end default true, hard max_count; closed seam not repeated.

## Mathematical specialization

Per subpath emit0,delta,2delta,... strictly below true L; optionally append open L once.
L0 emits one valid point. Determine count from certified true length, not its
already-rounded published approximation. Invert as PTH-04B.

## Runtime implementation

`path_arc_sampling.cpp` implements the strict key using certified true-length comparisons for sample count and inversion. `max_count` defaults to 4096 and remains a hard capacity. Its integration/refinement limits match PTH-04B. The spacing input and `include_open_end` affect the mathematical target; neither table tolerance nor rounded total length determines the count.

Retain each absolute target as the exact rational `j*spacing` through count comparison, inversion and `arc_s` rounding. Exact analytic length relations from PTH-04B resolve integer-count and endpoint ties. An appended open endpoint uses the true total length, independently of the next spacing multiple.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; allocate from the statically inferred max_count capacity, determine effective count by refining true-length comparisons, and report an operational capacity failure if the constructed result exceeds that capacity.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

L100/delta30 emits0,30,60,90,100 when open; closed excludes100; exact multiples and
near-boundary lengths.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
