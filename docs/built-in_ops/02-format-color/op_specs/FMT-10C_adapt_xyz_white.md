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
  - color.adapt_xyz_white_accelerated_apple_silicon
  - color.adapt_xyz_white_accelerated_x86_64
---

# FMT-10C: adapt an XYZ reference white

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This specification remains Proposed; the keys are registered fixed invocation ABI 8
operations under the strict, accelerated Apple Silicon and accelerated x86-64
CPU profiles.

## Interface and matrix

One input Result with one Float32 or Float64 tensor produces one
`values` output Result containing one tensor. The output preserves Result schema
identity, tensor key, logical shape and batch axes; it carries no description and sets `atomic_trailing_axes` to zero. Complete sample rank,
including batch and cell axes, is at most 8; sample count is at most 2^40.
Semantic input selects
one complete XYZ group and resolves source white from explicit input_description;
raw selects an ordered triple and explicit source white. Target white and method
are always explicit. The method is xyz_scaling, bradford, cat02 or cat16 with
full adaptation. It has no degree, surround, peak-luminance or user matrix input.

Normalize source and target whites to Y=1. Construct the exact matrix
K=inverse(H)*diag((H*Wt)/(H*Ws))*H. Require every response of both whites strictly
positive in all modes before selecting identity or evaluating samples. The numerical transform
preserves the reference/unit scale and adapts to the target white. It does not
ensure unchanged Y for arbitrary colored samples. Callers provide the target
configuration downstream; no description is attached in either path.

For nonidentity K, evaluate each row in the retained output tiles as one exact dot plus +0 before
rounding, with all three source coordinates read. Do not evaluate and round an
intermediate cone-space image. Source samples may be signed/HDR; positivity of
white parameters is not a sample constraint.

## Identity, scope and resources

Exact K=I uses bit-copy identity after all static parameter checks. Equal source
and target whites yield this case. Each retained coordinate reads its matching source sample; raw retains all bits, including signaling NaNs and signed zeros. Semantic callers guarantee finite selected samples copied into retained tiles. A method with invalid white responses
cannot become legal by asking for identity, Empty or alpha-only output.

Auto/view/materialize, complete output tile coverage, allocation ownership and scratch preparation
follow the family. Nonidentity XYZ Scaling is still a full three-input row under
NUM special-value semantics, despite its diagonal coefficients. Fixed input tile closure retains those peers. No channel-dependent approximation, hidden
fallback method or metadata-only adaptation is permitted.

## Independent fixtures

1. Let Ws=(1,1,2), Wt=(2,1,1), obtained exactly from xy=(0.25,0.25) and
   (0.5,0.25). Both whites have positive responses for all four methods.
   Every selected full-adaptation K maps the exactly representable input Ws to
   Wt exactly in Float32/64. XYZ Scaling is diag(2,1,0.5), mapping [3,-2,4]
   to [6,-2,2] exactly.
2. Verify K_reverse*K=I as rational matrices, but test the floating reverse
   composition against per-node rounded oracles instead of exact round-trip bits.
3. Bradford with white xy=(0.05,0.05) has a negative first response although
   the XYZ white is structurally positive. Reject the parameter in semantic/raw
   mode, even if source and destination whites are equal. Do not substitute
   XYZ Scaling or epsilon. Method-specific rejection is observable.
4. For valid equal whites, raw source [NaN,-0,1] queried only at the first
   component preserves its exact NaN bits. Semantic request of only the third
   returns 1 when its complete output tile excludes the first component. Including the first component violates the semantic finite-input precondition.
5. For unequal-white XYZ Scaling, a requested X with NaN Y must read it and
   return quieted NaN in raw; this sample is outside the semantic precondition. No static-zero peer pruning.
6. With absolute input 100*Ws, the result is 100*Wt under all four methods;
   reference-white Y remains 100 cd/m² without a peak-luminance parameter.
   Other non-neutral Y values need not be preserved by Bradford/CAT matrices.

Use the installed `format::adapt_xyz_white` helper to append C with explicit
source/target whites and method. This contract does not claim bit compatibility
with a rounded ICC chad tag or a full appearance model.
