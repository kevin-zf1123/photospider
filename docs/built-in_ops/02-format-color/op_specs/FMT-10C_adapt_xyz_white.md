---
spec_schema_version: 1
id: FMT-10C
parent_id: FMT-10
function: adapt_xyz_white
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.adapt_xyz_white_strict
  - color.adapt_xyz_white_accelerated_apple_silicon
  - color.adapt_xyz_white_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10C: adapt an XYZ reference white

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This is a Proposed specification,
not a registered implementation or an alias of a retired operation.

## Interface and matrix

One same-dtype/same-shape tensor `input` yields `values`. Semantic input selects
one complete XYZ group and resolves source white from effective metadata;
raw selects an ordered triple and explicit source white. Target white and method
are always explicit. The method is xyz_scaling, bradford, cat02 or cat16 with
full adaptation. It has no degree, surround, peak-luminance or user matrix input.

Normalize source and target whites to Y=1. Construct the exact matrix
K=inverse(H)*diag((H*Wt)/(H*Ws))*H. Require every response of both whites strictly
positive in all modes before selecting identity or evaluating samples. Semantic
input/output preserves reference and units; it changes the selected XYZ group's
white only after the numerical transform. This does not ensure unchanged Y for
arbitrary colored samples. Raw changes numbers without claiming adapted metadata.

For nonidentity K, evaluate each requested row as one exact dot plus +0 before
rounding, with all three source coordinates read. Do not evaluate and round an
intermediate cone-space image. Source samples may be signed/HDR; positivity of
white parameters is not a sample constraint.

## Identity, scope and resources

Exact K=I uses bit-copy identity after all static parameter checks. Equal source
and target whites yield this case. Only requested components are read/validated;
raw retains all bits, including signaling NaNs and signed zeros. Semantic identity
still rejects requested nonfinite samples. A method with invalid white responses
cannot become legal by asking for identity, Empty or alpha-only output.

Auto/view/materialize, exact output coverage, normal owners and page preparation
follow the family. Nonidentity XYZ Scaling is still a full three-input row under
NUM special-value semantics, despite its diagonal coefficients. Dirty maps and
failure scope retain those peers. No channel-dependent approximation, hidden
fallback method or metadata-only adaptation is permitted.

## Independent fixtures and future public workflow

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
   returns 1 without reading NaN; requesting the first then fails.
5. For unequal-white XYZ Scaling, a requested X with NaN Y must read it and
   return quieted NaN in raw or fail semantically. No static-zero peer pruning.
6. With absolute input 100*Ws, the result is 100*Wt under all four methods;
   reference-white Y remains 100 cd/m² without a peak-luminance parameter.
   Other non-neutral Y values need not be preserved by Bradford/CAT matrices.

A future public example selects D65->D50 and an explicit method, verifies
metadata and selected pixel coordinates independently, and compares with a
rational oracle derived from the resolved RN64 whites. This contract does not
claim bit compatibility with a rounded ICC chad tag or a full appearance model.
