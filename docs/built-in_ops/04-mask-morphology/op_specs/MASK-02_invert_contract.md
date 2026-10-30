---
spec_schema_version: 1
id: MASK-02
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-02: Canvas-relative complement

Inherits the [MASK baseline](MASK_common_contract.md).

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-02A](MASK-02A_invert.md) | invert | Invert coverage on the current canvas |

## Mathematics

The one input can be Coverage or Binary. Floating binary values are a subset of Coverage. Complement is over D only; the complement of a finite canvas is not an infinite foreground outside D. Output uses the same canvas and dtype. No static parameters. Applying twice is mathematically identity but finite floating subtraction can lose information; do not cancel two inversions solely by their names. For example a tiny positive float can become 1 then 0. Floating binary complement is exactly involutive.

## References

Project-defined arithmetic/contract; inherited NUM/FMT baseline.
