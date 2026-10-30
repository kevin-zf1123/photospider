---
spec_schema_version: 1
id: GEN-02B
parent_id: GEN-02
function: coordinate_axis
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.coordinate_axis_v1_strict
  - generation.coordinate_axis_v1_accelerated_apple_silicon
  - generation.coordinate_axis_v1_accelerated_x86_64
---

# GEN-02B: coordinate axis

Inherits [GEN-02](GEN-02_coordinate_grid_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.coordinate_axis_v1_strict`, `generation.coordinate_axis_v1_accelerated_apple_silicon`, `generation.coordinate_axis_v1_accelerated_x86_64`.

No Tensor inputs; values[H,W] Float32/64.

### Parameters and domain

GEN-02A parameters plus axis=x/y; constructor default x.

## Mathematics

Return only the selected GEN-02A component with identical bits; do not append a component axis or evaluate the other component.

## Behavior

Support class: Regional.

## Acceptance

Component projection agrees with GEN-02A; empty request does not read payload.
