---
spec_schema_version: 1
id: MASK-01
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-01: Mask algebras

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-01A](MASK-01A_binary_logic.md) | binary_logic | Binary Boolean operations |
| [MASK-01B](MASK-01B_fuzzy_logic.md) | fuzzy_logic | Fuzzy mask operations |
| [MASK-01C](MASK-01C_independent_coverage.md) | independent_coverage | Independent-coverage operations |

## Normative shared mathematics and interface

All three members have same-size operands `a,b` and a required static operation
in `and|or|xor|subtract`. `subtract` means A without B, not an unrestricted signed
subtraction. Do not mix algebras implicitly; no generic unsuffixed dispatcher is
introduced. Coverage is not subpixel geometry. Independent coverage describes
an independence assumption, not knowledge of actual overlap.

| Member | AND | OR | XOR | SUBTRACT |
| --- | --- | --- | --- | --- |
| A binary | a && b | a \|\| b | a != b | a && !b |
| B fuzzy | min(a,b) | max(a,b) | abs(a-b) | min(a,1-b) |
| C independent | a*b | a+b-a*b | a+b-2*a*b | a*(1-b) |

A has exact 0/1 outputs in the operands' dtype. B extrema select an operand
exactly; equal extrema choose a, including its signed zero. Fuzzy subtract
computes the whole formula once, not `RN(1-b)` before min. C and B's arithmetic
branches round the complete expression once. All outputs remain in [0,1].
For nonempty Q both operand samples at Q are Data+Validation, even where Boolean
short-circuiting could avoid b. Cost O(|Q|), O(1) arithmetic scratch per lane.
CPU SIMD must preserve ties and exact domains. Use mask set operations before
feathering; use vector-path Boolean before rasterizing for exact geometry.

## Sources, independent evidence and review boundary

[S01](../research-sources.md#s01)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
