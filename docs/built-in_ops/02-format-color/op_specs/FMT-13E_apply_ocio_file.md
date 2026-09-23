---
spec_schema_version: 1
id: FMT-13E
parent_id: FMT-13
function: apply_ocio_file
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_file_transform_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13E: frozen file transform

Inherit [FMT-13](FMT-13_ocio_transform_contract.md). E applies an admitted
v2.5.2 FileTransform reader to immutable resources, including CLF/CTF and LUT
formats. It is a Proposed native engine adapter, not CRV LUT interpolation.

## File and coordinate contract

Require frozen file bytes/logical path and complete forward entry_space/exit_space
declarations. Add a frozen config/context when symbolic dependencies require it;
otherwise use the family's explicit empty resolver. No runtime filesystem path
is an independently mutable resource. Retain reader-required format/version and
filename information in the frozen logical resource mapping.

direction defaults forward. Inverse must be constructible by the engine and may
be approximate/lossy. interpolation defaults default; default/best resolve to the
actual reader choices. Concrete nearest/linear/tetrahedral is accepted only where
the affected LUT stages support it. Reject unsupported cubic or other modes and
do not accept a warning-based substitution for a concrete request. Compound
files retain non-LUT stages and any file-defined range/encoding operations.
An explicit ccc_id selects a record where supported; empty follows the pinned
reader's default and the construction trace records the actual record.

The file's own encoded bit-depth/scale declarations are interpreted by the
reader; public coordinates remain the declared endpoint coordinates with the
family's only automatic public dtype adaptation RN32/widen. Do not infer that
a file containing an ICC section implements the FMT-12 CMM contract or admits
four-channel CMYK. The same three-component/alpha-independent admission applies.

## Execution and acceptance

One input/output tensor preserves dtype/shape/slots. Finite samples, exact point
support, requested-output validation, optimized/reference and ownership follow
the family. Entry/exit labels do not add transforms. Raw does not automatically
publish target semantics. Materialize requested output; view is forbidden.

- Independently constructed identity, constant and nontrivial 1D/3D LUTs check
  units, interpolation and engine inverse behavior. Include CLF/CTF with a
  non-LUT stage and explicit range processing.
- Record default/best effective interpolation; concrete unsupported mode fails
  rather than producing a different interpolation result. Missing record IDs
  and unreadable/malformed resources fail.
- Two frozen packages with the same logical filename and different LUT bytes
  must not cross-hit caches. A missing proxy resource must not read a real host
  file at the same path. Allocation/cancel exceptions cannot cause fallback.
- Check alpha admission, finite narrowing overflow, hidden colors, bypass,
  exact cross-tile requests and both execution configurations.

A future public workflow binds the frozen file and endpoint interpretation,
applies E to a small color grid and compares independent plus direct-engine
oracles. Neither runtime support nor measured precision is claimed here.
