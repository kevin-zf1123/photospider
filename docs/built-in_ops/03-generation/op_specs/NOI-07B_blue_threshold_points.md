---
spec_schema_version: 1
id: NOI-07B
parent_id: NOI-07
function: blue_threshold_points
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
implementation_scope: generic_rank_resource
qualified_asset_status: not_provided
operation_keys:
  - noise.rank_threshold_points_v1_strict
---

# NOI-07B: blue threshold points

Inherit [NOI-07](NOI-07_blue_noise_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Runtime implementation

The registered CPU key `noise.rank_threshold_points_v1_strict` implements generic
rank thresholding. Descriptor parameters follow NOI-07A. Height, width and
`threshold_count` are required; origins/offsets default to zero, `max_count`
defaults to 1,048,576 and `max_work` bounds actual work. There is no dtype parameter:
`points` is the canonical DynamicPoints v1 bundle. A complete-bundle task fills
positions/ordinal IDs/count and zero padding. A zero threshold reads no rank payload.
Capacity exhaustion returns ResourceExhausted/CapacityLimit without a successful
prefix. No named blue-quality or GPU key is registered.

## Interface and representation

Rank/resource as NOI-07A; [DynamicPoints v1](GEN-08_point_distributions_contract.md) Result, at most one point per canvas pixel.

## Parameters and domain

canvas,offsets,threshold_count K=0..N; nonnegative static hard output capacity max_count, with physical P=max(1,max_count). Zero capacity permits an empty result only; generating a selected point beyond the logical capacity fails operationally.

## Mathematical specialization

Select rank<K, publish global pixel centers RN64 in y/x scan order with ordinal IDs. One
complete resource period yields exactly K points; cropped/repeated coverage changes
count. K=0 is empty. No continuous Poisson-disk or minimum-distance claim.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

count/fill the selected canvas support without a resource-validity scan, O(HW); capacity exhaustion fails. This capacity is not
GEN-08C/D's normal count target.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Full-period counts, nested thresholds, crop/origin invariance and capacity rejection.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
