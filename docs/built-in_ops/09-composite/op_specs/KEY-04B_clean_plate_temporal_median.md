---
spec_schema_version: 1
id: KEY-04B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-04
function: clean_plate_temporal_median
execution: tile
proposed_operation_keys:
  - keying.clean_plate_temporal_median
---

# KEY-04B: Background plate from aligned frames

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.clean_plate_temporal_median`.

## Interface

`images: T[N,H,W,3]`, `valid: UInt8[N,H,W]` -> `plate: T[H,W,3]`, `confidence: T[H,W]`.

N>=1. Inputs are spatially aligned and share an explicit RGB interpretation. valid is binary; no motion alignment is performed.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

At each pixel gather valid original frame samples. Sort each channel by (value,frame_index). An odd count selects the middle stored value; an even count returns the average of the two central values. confidence=valid_count/N.

## Behavior

Each output pixel depends on that location across N frames. Plate color requires at least one valid frame and otherwise has NoSolution. Confidence-only output reads validity flags and returns 0 for no valid samples.

## Acceptance

Two valid channel samples 0.25 and 0.75 produce 0.5. A single valid sample is copied; confidence reflects its fraction of N.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
