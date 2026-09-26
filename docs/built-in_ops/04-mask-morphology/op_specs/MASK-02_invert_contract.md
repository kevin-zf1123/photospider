---
spec_schema_version: 1
id: MASK-02
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-02: Canvas-relative complement

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-02A](MASK-02A_invert.md) | invert | Invert coverage on the current canvas |

## Normative shared mathematics and interface

The one input can be Coverage or Binary. Floating binary values are a
subset of Coverage. Complement is over D only; the complement of a finite
canvas is not an infinite foreground outside D. Output uses the same canvas
and dtype. No static parameters. Time O(|Q|), constant scratch. Applying twice
is mathematically identity but finite floating subtraction can lose information;
do not cancel two inversions solely by their names. For example a tiny positive
float can become 1 then 0. Floating binary complement is exactly involutive.

## Sources, independent evidence and review boundary

Project-defined arithmetic/contract; inherited NUM/FMT baseline.

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
