---
spec_schema_version: 1
id: FMT-13F
parent_id: FMT-13
function: apply_ocio_transform_tree
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_transform_tree_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13F: explicit transform tree

Inherit [FMT-13](FMT-13_ocio_transform_contract.md). F is one native processor
for a fixed declarative Transform or GroupTransform tree. It is not a composite
workflow of separately published NUM/CRV/FMT tensors. Proposed/unimplemented.

## Tree and endpoints

Require a finite acyclic typed tree, complete forward entry_space/exit_space
declarations and any frozen config/context/files used by symbolic nodes. Pin
every node type and static parameter to v2.5.2 support. Empty GroupTransform is
valid. Record explicit node directions plus outer direction=forward/inverse;
the engine's composition/inverse rules determine order. Failure to build an
inverse is an error, not a request to ignore an offending child.

Resolve dynamic properties to immutable per-instance values before processor
optimization and require the final processor non-dynamic. All nested resources,
reference bridges, configured bypass and alpha effects are checked under the
family contract. A MatrixTransform that mixes alpha is disallowed even if a
later stage appears to cancel it. A normal RGB cross-channel matrix is allowed.
The declaration cannot embed executable callbacks, arbitrary plugins or mutable
external object pointers.

Entry/exit descriptions authorize interpretation and are not additional numerical
transforms. Engine GroupTransform evaluation has no project tensor rounding
between children; its own finalization/optimization remains observable. Do not
merge independent project nodes into F or split F into nodes without proving
all observable arithmetic, validation, demand and failure behavior equivalent.

## Samples and acceptance

One input/output tensor retains dtype/shape/slots, with the family F32 engine
boundary and bit-preserving alpha/AOV bypass. Materialize exactly requested
regions. Any transformed output consumes the full source triple. Raw shares the
finite boundary but does not claim the declared semantic target.

- Build an independent matrix/range/exponent tree and verify ordered processing
  against simple analytic anchors and the selected engine point oracle.
- Compare empty tree on Float64 with RN32/widen, including `1+2^-30 -> 1` and
  finite overflow rejection; no empty-tree view shortcut is admitted.
- Reject cyclic/deep-over-budget trees, unsupported nodes, missing dependencies,
  unavailable inverse, alpha writes/dependencies and unresolved dynamic state.
- Two same-type dynamic property instances with different explicit snapshots
  remain independently fixed, with no cross-instance sharing changing values.
- Exercise nested display/color-space bridges, configured bypass, sparse ROI,
  both configurations and cancellation within resource-heavy construction.

The future public example builds a typed tree and checks a small planar sample
grid through the installed public API. No current executable support is claimed.
