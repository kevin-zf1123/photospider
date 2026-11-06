---
spec_schema_version: 1
id: KEY-04A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-04
function: clean_plate_nearest
execution: tile
proposed_operation_keys:
  - keying.clean_plate_nearest
---

# KEY-04A: Nearest known background plate

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.clean_plate_nearest`.

## Interface

`image: T[H,W,3]`, `known: UInt8[H,W]` -> `plate: T[H,W,3]`, `confidence: T[H,W]`.

Explicit common RGB interpretation. known is binary 0/1; at least one known pixel is needed for a plate value.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The resolved color sample rank is three; removing the component axis yields `[H,W]`. The component extent is three except for edge extension, which also carries alpha and optional other channels.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Known pixels copy the input. At each unknown pixel, select the original known pixel minimizing (squared Euclidean integer distance, donor_y, donor_x) and copy its RGB. confidence is 1 at originally known locations and 0 elsewhere.

### Current implementation profile

The implementation uses a separable exact integer nearest-donor transform. A bounded horizontal candidate search is accepted only when unvisited columns cannot win or tie; all other rows use an exact parabola envelope. AArch64 compact-index packets first test seven columns against a strict squared-distance bound of 16, then add the two outer columns when needed before applying the bound of 25. Uncertified pixels receive a bounded seventeen-column check over the saved vertical donors; only a strict squared-distance bound of 81 certifies that result, otherwise the exact envelope replaces the whole row. Distance and row-major ties are integer decisions. Plate RGB values are copied bit for bit. Confidence is evaluated independently from known flags.

## Behavior

W support on the binary known mask and original donor colors. A requested plate with no donor has NoSolution under the common error convention. confidence depends only on the local known flag and remains defined without donors.

## Acceptance

One known red pixel fills the plate with red; confidence is 1 only at that pixel. Equal-distance donor ties use y then x.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
