---
spec_schema_version: 1
id: KEY-04B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
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

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The resolved sample rank is four, with the leading frame axis fixed and distinct from the three-component color axis. Removing the color axis yields `[N,H,W]`; removing the frame axis yields the plate layout. N is an input extent, with N>=1.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

At each pixel gather valid original frame samples. Sort each channel by (value,frame_index). An odd count selects the middle stored value; an even count returns the average of the two central values. confidence=valid_count/N.

### Current implementation profile

The frame count is the leading input extent, not a static parameter. Packed stacks with at most 16 frames use a collective implementation; larger or nonpacked stacks use the general path. Three fully valid frames use a stable three-comparison selection network. Other sample counts use selection ordered by (value, frame_index) and an overflow-safe midpoint for even counts. These choices do not restrict the declared frame-count domain. The collective pass records whether any plate pixel had no evidence. Only then does it recheck the requested plate region for NoSolution; confidence-only evaluation reads validity flags.

## Behavior

Each output pixel depends on that location across N frames. Plate color requires at least one valid frame and otherwise has NoSolution. Confidence-only output reads validity flags and returns 0 for no valid samples.

## Acceptance

Two valid channel samples 0.25 and 0.75 produce 0.5. A single valid sample is copied; confidence reflects its fraction of N.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
