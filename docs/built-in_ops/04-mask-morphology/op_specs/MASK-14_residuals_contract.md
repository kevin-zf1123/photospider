---
spec_schema_version: 1
id: MASK-14
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-14: Morphological gradients and hats

Inherits the [MASK baseline](MASK_common_contract.md).

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-14A](MASK-14A_morph_gradient.md) | morph_gradient | Morphological gradient |
| [MASK-14B](MASK-14B_inner_border.md) | inner_border | Inner morphological border |
| [MASK-14C](MASK-14C_outer_border.md) | outer_border | Outer morphological border |
| [MASK-14D](MASK-14D_white_top_hat.md) | white_top_hat | White top hat |
| [MASK-14E](MASK-14E_black_top_hat.md) | black_top_hat | Black top hat |

## Mathematics

Use Coverage or Binary, the same B/zero-lattice/stage rules as MASK-05/06. Let I=input, D=dilate, E=erode, O=opening, C=closing, each with the complete normative stage boundaries and no intermediate crop. A gradient=D(I)-E(I); B inner=I-E(I); C outer=D(I)-I; D white_top_hat=I-O(I); E black_top_hat=C(I)-I. Each difference is constructed exactly from the already specified extrema-selected operands, then RN_T once (Binary exact 0/1). The zero-extended lattice opening/closing guarantees these differences are nonnegative and <=1 for valid masks. No abs or silent clip is needed. An operator using cropped closing could produce negative black-hat residuals near an edge; that is a different signed Field contract, not this member.

All masks are nonnegative residuals, not signed Laplacians or derivative fields. Identity B={(0,0)} gives exact generated +0 for all five members under the unchecked input premise; it adds no validation scan. A/B/C require support Q+B (I is included by origin-containing B). D/E require Q+B+B. No support elision on a found zero. Every specified selection and rounding stage is preserved; these members are not aliases of a library morphologyEx.

## References

[S05](../research-sources.md#s05); [S06](../research-sources.md#s06); [S18](../research-sources.md#s18)
