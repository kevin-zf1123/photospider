---
spec_schema_version: 1
id: PTH-01A
parent_id: PTH-01
function: make_core_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.make_core_path_v1_strict
---

# PTH-01A: make core path

Inherits [PTH-01](PTH-01_construction_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.make_core_path_v1_strict`.

Ordered verbs, control_offsets, Float64 controls[C,2], subpath_offsets, UInt8 closed, Int64 counts[3] as six explicit input tensors; CoreVerbs PathSet follows the six-member photospider.ops.path_core v1 directory in the shared geometry contract.

### Parameters and domain

pixel-xy-right-down coordinates; schema segment/control limits. Empty paths use positive static-capacity Tensor members and ordinary payload counts equal to zero, not dynamic or zero-extent tensors. Capacities and canonical padding follow the shared Core schema; counts is an explicit input.

## Mathematics

M/L/Q/C/Z arities1/1/2/3/0, valid offsets and terminal Z/closed consistency are unchecked caller premises; copy control bits, preserve zero-length segments. Canonical empty offsets=[0]. Width and source-map Results are supplied through separate workflows; this constructor does not attach metadata.

## Behavior

Support class: complete input.

## Acceptance

M(0,0),L(3,4), M-only path, canonical empty data, valid offsets/associations and closed paths.
