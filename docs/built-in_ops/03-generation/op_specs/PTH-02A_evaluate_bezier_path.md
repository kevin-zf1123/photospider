---
spec_schema_version: 1
id: PTH-02A
parent_id: PTH-02
function: evaluate_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_bezier_path_v1_strict
  - path.evaluate_bezier_path_v1_accelerated_apple_silicon
  - path.evaluate_bezier_path_v1_accelerated_x86_64
---

# PTH-02A: evaluate bezier path

Inherits [PTH-02](PTH-02_evaluation_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.evaluate_bezier_path_v1_strict`, `path.evaluate_bezier_path_v1_accelerated_apple_silicon`, `path.evaluate_bezier_path_v1_accelerated_x86_64`.

controls[S,d+1,2],segment_indices[N],t[N]; position[N,2],derivative[N,2],tangent[N,2],UInt8 tangent_valid[N].

### Parameters and domain

Int64 degree 1/2/3; Float32/64 controls/query, explicit output dtype; t in [0,1], positive Tensor extents. Degree determines the static control shape. Check the original Int64 value against 1..3 before any narrowing conversion, including during empty-request preflight.

## Mathematics

B(t)=sum binom(d,i)*(1-t)^(d-i)*t^i*P_i; derivative=d*sum B_i^(d-1)*(P_(i+1)-P_i). Round complete component expressions once. Endpoints copy/convert controls. Interior exact zero preserves -0 only when every control in that component is -0. Tangent validity uses exact derivative, not rounded derivative.

## Behavior

Support class: Regional.

## Acceptance

3-4-5 line midpoint/derivative/unit tangent, cubic endpoint derivatives, exact-zero tangent and reversal identity. Static degree checks reject 0, 4, 2^32+1, 2^32+2, 2^32+3 and negative values even if their low 32 bits match a supported degree and the supplied control shape matches that truncated value.
