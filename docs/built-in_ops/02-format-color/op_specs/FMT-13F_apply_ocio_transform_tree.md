---
spec_schema_version: 1
id: FMT-13F
parent_id: FMT-13
function: apply_ocio_transform_tree
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_transform_tree_2_5_2_cpu
---

# FMT-13F: explicit transform tree

Inherits [FMT-13](FMT-13_ocio_transform_contract.md). F is one processor for a fixed declarative Transform or GroupTransform tree, not a composite workflow of separately published NUM/CRV/FMT tensors. The public ocio_transform_tree helper seals a declarative YAML tree.

## Interface

A finite acyclic typed tree, complete forward entry_space/exit_space declarations and any frozen config/context/files used by symbolic nodes are required. Every node type and static parameter is limited to v2.5.2 support. An empty GroupTransform is valid. Explicit node directions plus outer direction=forward/inverse are recorded; the engine's composition/inverse rules determine order. Failure to build an inverse is an error, not a request to ignore an offending child.

Dynamic properties resolve to immutable per-instance values before processor optimization and the final processor is non-dynamic. All nested resources, reference bridges, configured bypass and alpha effects are checked under the family contract. A MatrixTransform that mixes alpha is disallowed even if a later stage appears to cancel it; a normal RGB cross-channel matrix is allowed. The declaration cannot embed executable callbacks, arbitrary plugins or mutable external object pointers.

Entry/exit descriptions authorize interpretation and are not additional numerical transforms. GroupTransform evaluation has no tensor rounding between children; the engine's own finalization/optimization is observable. Independent nodes are not merged into F, nor F split into nodes, without proving equivalent arithmetic, validation and demand.

## Behavior

One input/output tensor retains dtype/shape/slots, with the F32 engine boundary and bit-preserving alpha/AOV bypass. Output is materialized. Any transformed output consumes the full source triple. Raw shares the finite boundary but does not claim the declared semantic target.

## Acceptance

- An independent matrix/range/exponent tree: ordered processing against simple analytic anchors and the point oracle.
- An empty tree on Float64 yields RN32/widen, including `1+2^-30 -> 1`; no empty-tree view shortcut exists.
- Cyclic trees, unsupported nodes, missing dependencies, unavailable inverse, alpha writes/dependencies and unresolved dynamic state are rejected.
- Two same-type dynamic property instances with different explicit snapshots remain independently fixed.
- Nested display/color-space bridges, configured bypass, sparse ROI and both configurations.
