---
spec_schema_version: 1
id: FMT-13E
parent_id: FMT-13
function: apply_ocio_file
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_file_transform_2_5_2_cpu
---

# FMT-13E: frozen file transform

Inherits [FMT-13](FMT-13_ocio_transform_contract.md). E applies an admitted v2.5.2 FileTransform reader to immutable resources, including CLF/CTF and LUT formats. It is not CRV LUT interpolation.

## Interface

Frozen file bytes/logical path and complete forward entry_space/exit_space declarations are required. A frozen config/context is added when symbolic dependencies require it; otherwise the family's explicit empty resolver applies. No runtime filesystem path is an independently mutable resource. Reader-required format/version and filename information are retained in the frozen logical resource mapping.

direction defaults forward. Inverse must be constructible by the engine and may be approximate/lossy. interpolation defaults to default; default/best resolve to the actual reader choices. Concrete nearest/linear/tetrahedral is accepted only where the affected LUT stages support it; unsupported cubic or other modes are rejected, with no warning-based substitution for a concrete request. Compound files retain non-LUT stages and any file-defined range/encoding operations. An explicit ccc_id selects a record where supported; empty follows the pinned reader's default and the actual record is recorded.

The file's own encoded bit-depth/scale declarations are interpreted by the reader; public coordinates remain the declared endpoint coordinates with RN32/widen as the only automatic public dtype adaptation. A file containing an ICC section does not implement the FMT-12 CMM contract or admit four-channel CMYK. The same three-component/alpha-independent admission applies.

## Behavior

One input/output tensor preserves dtype/shape/slots. Finite samples, exact point support, unchecked output premises and optimized/reference follow the family. Entry/exit labels add no transforms. Raw does not publish target semantics. Output is materialized; view is forbidden.

## Acceptance

- Independently constructed identity, constant and nontrivial 1D/3D LUTs check units, interpolation and engine inverse behavior, including CLF/CTF with a non-LUT stage and explicit range processing.
- Default/best effective interpolation is recorded; a concrete unsupported mode fails. Missing record IDs and unreadable/malformed resources fail.
- Two frozen packages with the same logical filename and different LUT bytes select their own immutable resources. A missing resource does not read a real host file at the same path.
- Structural alpha admission, valid finite narrowing, hidden colors, bypass, exact cross-tile requests and both execution configurations.
