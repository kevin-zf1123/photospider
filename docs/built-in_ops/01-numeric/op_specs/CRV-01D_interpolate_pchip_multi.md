---
spec_schema_version: 1
id: CRV-01D
parent_id: CRV-01
function: interpolate_pchip_multi
operation_family: curve.interpolate_pchip_multi
proposed_operation_keys:
  - curve.interpolate_pchip_multi_strict
  - curve.interpolate_pchip_multi_accelerated_apple_silicon
  - curve.interpolate_pchip_multi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-01D: interpolate_pchip_multi

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) and the single-function contract [CRV-01B](CRV-01B_interpolate_pchip.md) for all unchanged execution and acceptance rules.

## Interface

Dynamic inputs are x[K], y[K,C], query[N]; output is values[N,C]. All C functions share x knots and query positions; each y column is interpolated independently using the pchip mathematical function. Query order and the function-column axis are preserved, including C=1. Output has no inferred RGB, color, alpha or other image semantics. This is independent of the single-function [CRV-01B operation](CRV-01B_interpolate_pchip.md); one name never serves both rank-1 and rank-2 y/output forms. The public constructor is `interpolate_pchip_multi_node`.

Require 2<=K<=65536, N>=1, C>=1, K*C<=2^40 and N*C<=2^40. No separate small column limit applies. Both products are checked without overflow. x/query are rank-1 and y/values are rank-2, with y.shape[0]=x.shape[0]. Output shape [query.shape[0],y.shape[1]] and the static output dtype are inferred before reading payload. All three input ports are required, in x/y/query order; values is the sole output. Each port has one dtype, so y columns share that array's dtype; x, y and query may independently use Float32/Float64. Outputs select Float32/Float64, default Float64, with no attached domain description.

Required String `dtype` is float32/float64 and `out_of_domain` is reject/clamp/linear_extrapolate. Constructors write float64/reject respectively; direct nodes specify both. The static choices apply to the entire node; there is no per-column parameter list or method mode. Missing ports, rank/dtype/length mismatch and shape/product violations fail at compile/preflight. Scalar and vector forms are not implicitly interconverted.

## Mathematics and behavior

Each output values[i,c] has the same formula, dtype rules, finite-value policy, domain policy, precision and zero handling as CRV-01B at query[i] using the column y[:,c]. Output row i, column c depends only on x, query[i] and the stencil of column y[:,c]; unselected query rows and columns do not enter arithmetic. The output column axis is ordinary, including C=1. Region and tile results equal the corresponding slice of the whole result. Caller preconditions follow the [CRV-01 family contract](CRV-01_interpolate.md#behavior).

## Acceptance

Fixture: x=[0,1,2], y=[[0,4],[1,3],[4,0]], query=[0.5,1.5] -> strict values=[[0.3125,3.6875],[2.1875,1.8125]]. The second column is 4 minus the first function. Accelerated profiles are validated for ULP, per-column shape/monotonicity and exact node/zero behavior against the exact PCHIP reference.

Every selected column is compared with the matching single-function operation, including mixed input dtype, both destinations, C=1 without squeezing, all domain policies and signed zeros.

## References

- [Family specification](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).
