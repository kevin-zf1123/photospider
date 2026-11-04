---
spec_schema_version: 1
id: GRD-21A
parent_id: GRD-21
function: saturation_linear
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.saturation_linear
---

# GRD-21A: Linear-RGB saturation

Inherits the [family contract](GRD-21_saturation_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.saturation_linear` performs the transformation below.

## Interface

`input: T[S,C]`, `scale: Float64[1]` -> `values: T[S,C]`.

Select linear RGB with explicit basis; scale>=0, suggested 1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Y is the working-basis Y row applied to RGB. Return Y+scale*(RGB-Y). scale=1 copies. Equal-channel neutral inputs copy directly.

The current native Float64 profile may prepare `q[c]=(1-scale)*w[c]` for the working-basis Y row w and evaluate `t=fma(q[2],B,fma(q[1],G,q[0]*R))`, followed by `fma(scale,RGB[c],t)`. Preparation requires positive scale and a finite conservative coefficient bound K<=32: form `W=up(up(abs(w[0])+abs(w[1]))+abs(w[2]))` and `K=up(scale+up(up(1+scale)*W))`, with `up(x)=nextafter(x,+infinity)`. Admitted controls use this same sequence in scalar, strided, tail and vector evaluations. Compute an upward enclosure of the L1 norm of the actual rounded q coefficients. A bound <=1/2 proves that t remains finite for finite RGB, without a pixel-domain check. Otherwise, a nonfinite t falls back for that pixel to the direct expression with `Y=(w[0]*R+w[1]*G)+w[2]*B`; finite neighbouring pixels retain the prepared expression. The fallback decision and arithmetic are identical in scalar and vector evaluation. Coefficients and guards are refreshed from the current invocation's controls before its row/chunk scans, with no cross-Run state. Other controls and Float32 computation retain the direct expression. Scale one and neutral triples retain their copy rules before output conversion. Equivalent native reassociation follows the unchanged common continuous comparison criteria; no universal conditioning bound for arbitrary extreme signed/HDR inputs is introduced.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

scale=0 produces a neutral triple at Y. scale=1 is identity; neutral input stays neutral for all scales.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
