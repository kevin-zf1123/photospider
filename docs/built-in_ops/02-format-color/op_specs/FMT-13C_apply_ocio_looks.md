---
spec_schema_version: 1
id: FMT-13C
parent_id: FMT-13
function: apply_ocio_looks
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_looks_2_5_2_cpu
---

# FMT-13C: explicit Look sequences

Inherits [FMT-13](FMT-13_ocio_transform_contract.md). C is one processor using LookTransform, not independently rounded nodes for each Look.

## Interface

Frozen config/context, explicit source_space/destination_space and an ordered Look sequence or ordered candidate sequences are required. Each item has an exact Look name and forward/inverse direction. An explicitly supplied empty sequence means only the source-to-destination conversion. Direction of the complete operation defaults forward; inverse exchanges effective endpoints, reverses each sequence and flips item directions while preserving candidate priority.

Engine process-space conversion stays enabled; C has no skipColorSpaceConversion shortcut. Process-space implicit reference bridges obey the family guard. The Look's authored forward/inverse definitions and pinned fallback construction determine the map. A configured process space is not replaced with the input's current analytic space.

Candidate fallback happens during construction on the frozen manifest and only for engine missing-file conditions. A missing Look or general error is fatal. A host resource failure is never an eligible missing-file condition. The chosen sequence is recorded and reused during execution; later filesystem changes cannot change it.

## Behavior

One input/output tensor retains dtype/shape/slots. The effective destination and Look configuration remain explicit parameters; output data carries no description. Family alpha admission, the F32 boundary and request/materialization rules apply to the whole chain.

## Acceptance

- Two simple affine Looks distinguish authored order from reversed order and verify complete inverse ordering. One-processor evaluation is compared to its own oracle; separately materialized Looks need not have identical rounding.
- Explicit inverse_transform preference and engine inversion when only the opposite authored direction is available. Unavailable inverses fail.
- A first candidate missing a LUT may select a second candidate. A missing Look or malformed LUT does not select that candidate. Adding the missing file outside the frozen package does not alter the result.
- Process-space crossing rejection/explicit bridge, empty sequence, names containing unaddressable engine grammar delimiters, alpha admission and F64 no-op narrowing; exact ROI and both execution configurations.
