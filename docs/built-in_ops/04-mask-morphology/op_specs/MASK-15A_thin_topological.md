---
spec_schema_version: 1
id: MASK-15A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-15
function: thin_topological
oracle_entry: thin_topological
registered_operation_keys:
  - mask.thin_topological_strict
---

# MASK-15A: Row-major topology-preserving binary thinning

Inherits the [family contract](MASK-15_topology_reconstruction_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.thin_topological_strict`.

input: Binary[H,W] -> skeleton: Binary[H,W]

Static parameters: None; fixed foreground8/background4 topology.

## Mathematics

Use immediate row-major simple-point deletions with endpoint and global topology checks, until fixed point.

complete-input Binary input and global iteration.

## Acceptance

single1 and a two-pixel chain survive; 2x2 full block must not vanish; a 1-pixel closed ring retains its hole.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
