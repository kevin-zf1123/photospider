---
spec_schema_version: 1
id: FMT-10C
parent_id: FMT-10
function: adapt_xyz_white
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.adapt_xyz_white_strict
  - color.adapt_xyz_white_fast
  - color.adapt_xyz_white_accelerated_apple_silicon
  - color.adapt_xyz_white_accelerated_x86_64
---

# FMT-10C: adapt an XYZ reference white

Inherits the [FMT-10 family](FMT-10_rgb_basis_contract.md) and [exact matrix definitions](FMT-10_basis_math.md). The keys are the strict, accelerated Apple Silicon and accelerated x86-64 CPU profiles.

## Interface

One input Result with one Float32 or Float64 tensor produces one `values` output Result containing one tensor. The output preserves Result schema identity, tensor key, logical shape and batch axes; it carries no description and sets `atomic_trailing_axes` to zero. Complete sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. Semantic input selects one complete XYZ group and resolves source white from the explicit input_description; raw selects an ordered triple and explicit source white. Target white and method are always explicit. The method is xyz_scaling, bradford, cat02 or cat16 with full adaptation. There is no degree, surround, peak-luminance or user matrix input.

## Mathematics

Source and target whites are normalized to Y=1. The exact matrix is K=inverse(H)*diag((H*Wt)/(H*Ws))*H. Every response of both whites is strictly positive in all modes (an unchecked premise, applying before identity selection). The transform preserves the reference/unit scale and adapts to the target white; it does not keep Y unchanged for arbitrary colored samples. Callers provide the target configuration downstream; no description is attached in either path.

For nonidentity K, each row is one exact dot plus +0 before rounding, with all three source coordinates read. No intermediate cone-space image is rounded. Source samples may be signed/HDR; positivity of white parameters is not a sample constraint.

## Behavior

Exact K=I uses bit-copy identity after all static parameter checks; equal source and target whites yield this case. Each output coordinate reads its matching source sample; raw retains all bits, including signaling NaNs and signed zeros. Semantic callers guarantee finite selected samples copied. A method with invalid white responses does not become legal by asking for identity, Empty or alpha-only output. Nonidentity XYZ Scaling is still a full three-input row under NUM special-value semantics, despite its diagonal coefficients; the zero-coefficient peers are read. There is no channel-dependent approximation, fallback method or metadata-only adaptation.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-10_rgb_basis_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

1. Ws=(1,1,2), Wt=(2,1,1), obtained exactly from xy=(0.25,0.25) and (0.5,0.25). Both whites have positive responses for all four methods. Every selected full-adaptation K maps the exactly representable input Ws to Wt exactly in Float32/64. XYZ Scaling is diag(2,1,0.5), mapping [3,-2,4] to [6,-2,2] exactly.
2. K_reverse*K=I as rational matrices; the floating reverse composition is tested against per-node rounded oracles, not exact round-trip bits.
3. Bradford with white xy=(0.05,0.05) has a negative first response although the XYZ white is structurally positive. This violates the selected method's numerical caller premise in semantic/raw mode, including equal source and destination whites. XYZ Scaling or an epsilon is never substituted.
4. For valid equal whites, raw source [NaN,-0,1] queried only at the first component preserves its exact NaN bits. Semantic request of only the third returns 1 when its complete output excludes the first component.
5. For unequal-white XYZ Scaling, a requested X with NaN Y reads it and returns quieted NaN in raw; this sample is outside the semantic precondition. No static-zero peer pruning.
6. With absolute input 100*Ws, the result is 100*Wt under all four methods; reference-white Y remains 100 cd/m^2 without a peak-luminance parameter. Other non-neutral Y values need not be preserved by Bradford/CAT matrices.

This contract claims no bit compatibility with a rounded ICC chad tag or a full appearance model.
