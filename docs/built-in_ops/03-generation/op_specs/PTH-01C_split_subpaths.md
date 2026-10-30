---
spec_schema_version: 1
id: PTH-01C
parent_id: PTH-01
function: split_subpaths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.split_subpaths_v1_strict
---

# PTH-01C: split subpaths

Inherits [PTH-01](PTH-01_construction_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.split_subpaths_v1_strict`.

PathSet and group_of_subpath[K] Int64 Result Tensor member; flat PathSet plus associated PathPartition Result.

The associated Result uses `photospider.ops.path_partition` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

Static group_count G>=1; group IDs in [0,G); preserve within-group order.

## Mathematics

Stable-group subpaths into one flat Result; partition offsets have G+1 entries and represent empty groups. No dynamic output-port count or nested Result. Remap global-width/source associations rather than copying stale normalized coordinates.

The v1 geometry schema contains no attached attribute members. These geometry-only ports do not consume separate width or source-map Results. Workflows carrying such Results must apply the association rules explicitly; selecting an attribute policy does not copy a separate Result implicitly.

## Behavior

Support class: complete input.

## Acceptance

groups1,0,1 yields source order1,0,2 and offsets0,1,3; empty group and valid group IDs.
