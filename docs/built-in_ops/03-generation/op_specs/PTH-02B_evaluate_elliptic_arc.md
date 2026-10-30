---
spec_schema_version: 1
id: PTH-02B
parent_id: PTH-02
function: evaluate_elliptic_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_elliptic_arc_v1_strict
  - path.evaluate_elliptic_arc_v1_accelerated_apple_silicon
  - path.evaluate_elliptic_arc_v1_accelerated_x86_64
---

# PTH-02B: evaluate elliptic arc

Inherits [PTH-02](PTH-02_evaluation_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.evaluate_elliptic_arc_v1_strict`, `path.evaluate_elliptic_arc_v1_accelerated_apple_silicon`, `path.evaluate_elliptic_arc_v1_accelerated_x86_64`.

Arc payload(c,u,v,theta0,sweep or FullTurn direction),segment_indices[N],t[N]; four evaluation outputs as PTH-02A.

### Parameters and domain

Schema-valid arc (nonorthogonal nondegenerate axes permitted), radians, t in [0,1], dtype; FullTurn direction +/-1.

## Mathematics

theta=theta0+t*sweep; P=c+u*cos(theta)+v*sin(theta); derivative=sweep*(-u*sin(theta)+v*cos(theta)). FullTurn uses exact direction*2*pi; at t=1 reuse t=0 position and derivative bits. No rounded2pi closure test.

## Behavior

Support class: Regional.

## Acceptance

Circle quadrants, ellipse nonconstant speed, FullTurn endpoint bit identity and huge angles.
