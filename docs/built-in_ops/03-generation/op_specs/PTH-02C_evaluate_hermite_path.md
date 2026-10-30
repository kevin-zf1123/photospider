---
spec_schema_version: 1
id: PTH-02C
parent_id: PTH-02
function: evaluate_hermite_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_hermite_path_v1_strict
  - path.evaluate_hermite_path_v1_accelerated_apple_silicon
  - path.evaluate_hermite_path_v1_accelerated_x86_64
---

# PTH-02C: evaluate hermite path

Inherits [PTH-02](PTH-02_evaluation_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.evaluate_hermite_path_v1_strict`, `path.evaluate_hermite_path_v1_accelerated_apple_silicon`, `path.evaluate_hermite_path_v1_accelerated_x86_64`.

controls[S,4,2]=(P0,P1,d0,d1),segment_indices[N],t[N]; four evaluation outputs.

### Parameters and domain

dtype; d0/d1 are derivatives with respect to normalized t, not unit tangents or relative handles.

## Mathematics

P=(2t^3-3t^2+1)P0+(t^3-2t^2+t)d0+(-2t^3+3t^2)P1+(t^3-t^2)d1. Differentiate exactly. Bezier conversion uses exact P0+d0/3 and P1-d1/3 intermediates unless separately published.

## Behavior

Support class: Regional.

## Acceptance

Endpoint derivative hits, straight-line derivatives, exact Hermite/Bezier algebra agreement, zero tangent validity.
