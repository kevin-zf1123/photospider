---
spec_schema_version: 1
id: GRD-02B
parent_id: GRD-02
function: lift_gamma_gain
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.lift_gamma_gain
---

# GRD-02B: Lift, gamma and gain

Inherits the [family contract](GRD-02_offset_gain_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.lift_gamma_gain` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,3]` with rows [lift,gamma,gain] -> `values: T[S,C]`.

Suggested rows [0,1,1]; gamma>0, gain>=0; lift is finite.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The current operation key and selector parameter types are specified in the [family parameter profile](GRD-02_offset_gain_contract.md#current-parameters). Controls are read from the current dynamic input on each invocation; the prepared operation retains no numeric controls payload.

## Mathematics

`t=(1-lift)*x+lift; y=gain*spow(t,1/gamma)`, where spow(t,a)=sign(t)*abs(t)^a and spow(0,a)=0. Identity controls copy. The signed extension preserves the sign through the power.

### Current native algorithm/profile

The implementation forms binary64 coefficients `scale=1-lift` and `power=1/gamma`, evaluates `t=fma(scale,Float64(x),lift)`, applies the signed power, then multiplies by gain. All calculation is binary64, including for Float32 images; conversion to the image element type occurs once at the end. Identity rows `[0,1,1]` and unselected components copy the original element bytes independently.

The native SIMD power path uses a 128-cell logarithm reduction, a binary64 log1p polynomial, and a range-reduced exponential polynomial chosen for the output tolerance. It admits finite normal `abs(t)`, `power <= 64`, and `abs(power*log2(abs(t))) <= 900`; exceptional values and the output-overflow boundary use scalar `std::pow` with sign restoration. `power==1` uses `t` directly. Zero follows the signed native arithmetic path. NEON and runtime-admitted AVX2/FMA share this profile, and noncontiguous layouts use bounded gathering or scalar evaluation. The common native-versus-strict tolerances remain unchanged.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Identity copies negative values. With lift=0, gamma=2, gain=1, x=-0.25 gives -0.5.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
