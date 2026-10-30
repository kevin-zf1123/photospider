---
spec_schema_version: 1
id: CRV-10B
parent_id: CRV-10
function: invert_pchip
operation_family: curve.invert_pchip
proposed_operation_keys:
  - curve.invert_pchip_strict
  - curve.invert_pchip_accelerated_apple_silicon
  - curve.invert_pchip_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-10B: invert_pchip

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Invert the mathematical PCHIP curve defined by x/y, before its forward output rounding. A rounded lookup is not inverted and no swapped-control interpolator is constructed. The [CRV-10 shared contract](CRV-10_invert.md) is normative for every interface, error, numerical and execution requirement.

## Interface

Ordered Result ports x[K],y[K],query[N] each contain one tensor member under any schema id/key and independently accept Float32/Float64; extents come from `sample_shape()`. K=2..65536,N=1..2^40. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], no attached domain description and Float32/Float64 dtype selected by static `dtype`, default Float64. The other required static String parameter out_of_domain is reject/clamp, default reject. The public constructor is `invert_pchip_node`.

## Mathematics

x is finite strictly increasing; y is finite strictly increasing or decreasing, without plateaus. No extrapolated tail is inverted. Endpoints correctly convert corresponding x, preserving zero sign. Use exactly the CRV-01B rational PCHIP slopes/polynomial, bracket the unique root and correctly round final x in strict. Accelerated permits <=4 ULP x error, exact classification/zero/endpoint rules, segment bounds and monotonicity. Residual alone is not the quality bound. Other mathematical exact zeros are +0 and nonzero underflow retains sign. An unreturned endpoint conversion does not invalidate a finite root.

## Behavior

Each output element depends on the complete x/y arrays and its own query value. Finite globally monotone controls, valid consumed queries and representable outputs are unchecked caller preconditions. Region and tile results equal the corresponding slice of the whole result.

## Acceptance

Fixture: x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> values=[0.5,1.5]. Negating y and query retains the result. CRV-10's exact reference, boundary/rounding/monotonicity checks apply.
