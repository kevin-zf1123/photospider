---
spec_schema_version: 1
id: KEY-07B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-07
function: matte_dilate
execution: tile
proposed_operation_keys:
  - keying.matte_dilate
---

# KEY-07B: Matte dilate

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.matte_dilate`.

## Interface

`matte: T[H,W]` -> `matte: T[H,W]`.

Input in [0,1]. Required static radius:Int64 in [0,4096], in pixels.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

`radius:Int64` is required and has the declared caller domain 0..4096.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Select the maximum original sample in the cropped square radius neighborhood. Equal values select the first global row-major sample, retaining its bits. Radius zero copies.

### Current implementation profile

The implementation separates horizontal and vertical cropped extrema. Radii one through five and radius eight use specialized row windows. Float32 radii four, five and eight use trailing power-of-two horizontal reductions and, when height permits, a ring of 2*radius+4 rows to evaluate four vertical windows together. Radius three selects a four-output row ring on non-x86 targets, rolling two/four/seven-row extrema for narrow x86 images, or the specialized single-output row window. The AVX2 Float32 radius-eight path repeats boundary rows to evaluate the same cropped extrema in four-row groups. All remaining radii use ordered block prefix/suffix extrema with block length 2*radius+1. Single-column images use the equivalent contiguous one-dimensional scan. Strict comparisons and ordered combines preserve the earliest global row-major source when equal extrema have different zero signs. Radius zero copies source bits.

## Behavior

H(radius) support relative to the full image. ROI/tile boundaries retain the original neighborhood.

## Acceptance

Radius zero is bit identity. A single one among zeros shrinks under erosion and expands under dilation.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
