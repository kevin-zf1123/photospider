---
spec_schema_version: 1
id: FMT-10B
parent_id: FMT-10
function: xyz_to_rgb
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.xyz_to_rgb_strict
  - color.xyz_to_rgb_accelerated_apple_silicon
  - color.xyz_to_rgb_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10B: XYZ to linear RGB

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This is a Proposed specification,
not a registered implementation or an alias of a retired operation.

## Interface and white handling

One `input` tensor Value produces unchanged dtype/shape `values`. Select a
complete XYZ group in semantic mode, or an explicit ordered raw triple. Target
RGB basis geometry is required. Construct its exact normalized M and use M^-1;
there is no implicit transfer, adaptation, exposure or gamut constraint.

Semantic white_handling defaults to require_match. Input XYZ white must match
the target RGB basis white. Explicit preserve_xyz allows a mismatch while using
the same target inverse matrix; it does not map source white to target white.
To adapt, call C first. Raw B ignores source color interpretation and does not
accept an unused white_handling field; explicit target geometry remains valid.

For each requested transformed row i, output RN_dtype(sum_j inverse(M)[i,j]*XYZ[j]+0).
A nonidentity request reads and validates all source X/Y/Z in semantic mode;
raw applies NUM's full three-term behavior. Source unit/reference scale stays
unchanged. Publish linear R/G/B into the original X/Y/Z slots with target basis,
source reference/units and no incompatible complete-space labels. Raw keeps
applicable source metadata without asserting RGB conversion.

## Dependencies, storage and errors

Inherit native exact-I copy exceptions, nonidentity tuple/bypass support,
pointwise or triple-fanout dirty mapping and auto/view/materialize. An explicit
preserve_xyz choice does not authorize a view of a nonidentity inverse matrix.
Default white mismatch and incompatible source/model/reference are preflight
errors; geometry and source assertions are checked even for Empty/bypass-only
requests. Negative/out-of-gamut RGB is legal; requested nonfinite semantic
input/output fails without clipping. Resource, cancellation and ownership rules
remain those of the family.

## Independent fixtures and future public workflow

1. The A custom-axis basis with white=(0.25,0.25) has inverse diag(1,1,0.5).
   XYZ [1,-2,6] maps to RGB [1,-2,3] exactly in both dtypes.
2. For actual floating XYZ samples near each preset white, compare to exact
   inverse-M evaluation. A nonrepresentable rational W cannot be injected as
   an exact floating sample to claim B(W)=[1,1,1] without input rounding.
3. Input white Ws=(1,1,2) from xy=(0.25,0.25), target custom-axis basis white
   Wt=(2,1,1) from xy=(0.5,0.25): default require_match rejects. With explicit
   preserve_xyz, input XYZ [1,1,2] yields RGB [0.5,1,2] exactly, showing that
   source white need not become neutral under this policy.
4. Precede the preceding target B with C mapping Ws to Wt: exact representable
   white input [1,1,2] then yields [1,1,1] for the chosen fully adapted white.
   Test the stages and metadata independently, including rejection without C.
5. A diagonal inverse row still consumes its zero-coefficient peers. A NaN Y
   makes a requested raw R NaN and semantic R fail. Alpha-only is an exact copy
   and requires no XYZ samples. Dirty changes to any selected XYZ component
   invalidate all observed nonidentity RGB rows at that pixel.

A future public workflow uses B's explicit target geometry and, for unequal
whites, either explicit preserve_xyz or a preceding C node. Connect FMT-09B
separately if encoded RGB is needed. No installed API or conformance result is
claimed by this Proposed member.
