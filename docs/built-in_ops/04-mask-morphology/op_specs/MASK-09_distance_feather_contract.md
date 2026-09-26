---
spec_schema_version: 1
id: MASK-09
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-09: Distance-domain feathering

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-09A](MASK-09A_distance_feather_linear.md) | distance_feather_linear | Linear signed-distance feather |
| [MASK-09B](MASK-09B_distance_feather_smoothstep.md) | distance_feather_smoothstep | Smoothstep signed-distance feather |

## Normative shared mathematics and interface

Input is a signed Distance with explicitly asserted distance unit and basis;
no-feature signed infinities use the explicit endpoint rules below. Inner/outer
widths are finite Float64>=0 in the same units. A true SDF has negative interior,
but explicitly tagged signed-center fields may also be used with their distinct
center-based meaning. No inferred contour reconstruction or reinitialization.

When both widths are zero return [d<=0]. Otherwise let t=(d+inner)/(inner+outer)
and u=clip01(t). A returns RN_T(1-u). B returns RN_T(1-u²*(3-2*u)). Exact
outside endpoints return 1/+0. One-sided zero widths are valid: inner=0 gives
coverage 1 at d=0; outer=0 gives coverage 0 at d=0. Thus the hard double-zero
branch is intentionally not a continuous limit from every one-sided family.
Symmetric positive widths give exactly .5 at d=0; asymmetric widths generally
do not. Sum and numerator are constructed exactly even if a floating sum would
overflow. Time O(Q), constant scratch; Data+Validation at Q only. This is not
Gaussian blur and does not automatically preserve the area of a selection.

## Sources, independent evidence and review boundary

[S12](../research-sources.md#s12)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.

Distance inputs admit signed no-feature infinities and reject NaN. Finite
shift preserves infinities; finite threshold and feather map -Inf to 1 and
+Inf to 0, as applicable. Finite arithmetic overflow remains an error.
