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

Inherits [PTH-04](PTH-04_resampling_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.resample_arc_spacing_v1_strict`.

PathSet,positive spacing[1] in px; PathSamplesArc Result.

The associated Result uses `photospider.ops.path_samples_arc` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

include_open_end default true, hard max_count; closed seam not repeated.

`max_count` defaults to 4096 and is a hard capacity; `max_refinements` defaults to 4096. The spacing and `include_open_end` determine the mathematical targets; neither a table tolerance nor a rounded total length decides the count.

## Mathematics

Per subpath emit0,delta,2delta,... strictly below true L; optionally append open L once. L0 emits one valid point. Determine the count from the true length, not its rounded published approximation. Each absolute target is the exact rational `j*spacing` through the count comparison, the inversion and the `arc_s` rounding; exact length relations from PTH-04B decide integer-count and endpoint ties. An appended open endpoint uses the true total length, independently of the next spacing multiple. Invert as PTH-04B.

## Behavior

Support class: complete input. A constructed result larger than `max_count` is a capacity failure.

## Acceptance

L100/delta30 emits0,30,60,90,100 when open; closed excludes100; exact multiples and near-boundary lengths.
