---
spec_schema_version: 1
id: FMT-13C
parent_id: FMT-13
function: apply_ocio_looks
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_looks_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13C: explicit Look sequences

Inherit [FMT-13](FMT-13_ocio_transform_contract.md). C is one native processor
using LookTransform, not independently rounded project nodes for each Look.
It is Proposed/unimplemented.

## Parameters and construction

Require frozen config/context, explicit source_space/destination_space and an
ordered Look sequence or ordered candidate sequences. Each item has an exact
Look name and forward/inverse direction. Empty explicitly supplied sequence
means only source-to-destination conversion. Direction of the complete operation
defaults forward; inverse exchanges effective endpoints, reverses each sequence
and flips item directions while preserving candidate priority.

Keep engine process-space conversion enabled; there is no skipColorSpaceConversion
shortcut in C's interface. Process-space implicit reference bridges obey the
family guard. The Look's authored forward/inverse definitions and pinned fallback
construction determine the map. Do not replace a configured process space with
the input's current analytic space.

Candidate fallback happens during construction on the frozen manifest and only
for engine missing-file conditions. A missing Look or general error is fatal.
Latched host resource/cancellation failure is never an eligible missing-file
condition. Record the chosen sequence, then reuse that resolved choice during
execution. Runtime filesystem changes cannot change it.

## Outputs and acceptance

One input/output tensor retains dtype/shape/slots. Semantic output describes the
effective destination, with Look provenance separately recorded. Raw retains
only applicable original unverified metadata. Family alpha admission, F32
boundary, request/materialization and accounting rules apply to the whole chain.

- Two simple affine Looks distinguish authored order from reversed order and
  verify complete inverse ordering. Compare one-processor evaluation to its own
  oracle; do not assume separately materialized Looks have identical rounding.
- Test explicit inverse_transform preference and engine inversion when only
  the opposite authored direction is available. Unavailable inverses fail.
- First candidate missing a LUT may select a second candidate. Missing Look,
  malformed LUT or host budget failure must not silently select that candidate.
  Adding the missing file outside the frozen package does not alter the result.
- Cover process-space crossing rejection/explicit bridge, empty sequence, names
  containing unaddressable engine grammar delimiters, alpha admission and F64
  no-op narrowing. Test exact ROI and both execution configurations.

The future public workflow explicitly supplies the ordered Look list and frozen
dependencies. This spec contains no claimed implementation or runtime result.
