---
spec_schema_version: 1
id: PTH-04B
parent_id: PTH-04
function: resample_uniform_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.resample_uniform_arc_v1_strict
---

# PTH-04B: resample uniform arc

Inherits [PTH-04](PTH-04_resampling_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.resample_uniform_arc_v1_strict`.

PathSet; PathSamplesArc Result with positions,source_segment,local_t,arc_s.

The associated Result uses `photospider.ops.path_samples_arc` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

count_per_subpath n>=1; hard max_count; open n1 takes start; closed endpoints not repeated.

`count_per_subpath` defaults to 2; `max_count` defaults to the subpath capacity times that count; `max_refinements` defaults to 4096. If the inverse-length or rounding certificate fails, the call fails; no table-interpolated parameter is substituted. The output schema is constructed by `generation::path_samples_schema` (arc variant).

## Mathematics

Open n>=2 targets s_j=jL/(n-1); closed uses jL/n. L0 repeats the valid start n times. Invert true length; interior positive-length junction chooses following segment t0, final open endpoint last valid segment t1. Position RN64(B(t*)) and local_t RN64(t*) are independent; do not reconstruct position using rounded t.

## Behavior

Support class: complete input.

## Acceptance

100px line five samples, closed400px square four corners, zero length and no inter-subpath jump length. Include `(0,0)->(1,1)->(2,2)` with count 3: the middle sample is `(1,1)` on the second line at `t=0`; also scaled/mixed square classes, zero-length junction runs, a near-but-unequal junction, common-pi circular lengths, and monotone quadratic/cubic representations with rational and irrational inverse parameters.
