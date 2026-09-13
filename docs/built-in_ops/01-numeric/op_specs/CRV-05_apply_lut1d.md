---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: legacy_subset_only_target_not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-05: apply_lut1d family

## Confirmed split

Provide two independent operations, both preserving input shape:

- Single-table application: table[L] maps every numeric element of input.
- Per-channel multi-table application: table[L,C] column c maps input[...,c].

Both target interfaces are fully clarified. Mapping one
scalar to a vector/color ramp is CRV-06 rather than this same-shape LUT family.
Both receive explicit axis[3], use fixed linear interpolation and inherit their
specified finite-value, exact-rounding and local-table-demand contracts. These
proposed operations do not claim runtime implementation.

- [Single-table draft](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
