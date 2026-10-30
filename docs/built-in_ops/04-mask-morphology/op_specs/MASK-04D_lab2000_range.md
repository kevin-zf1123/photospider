---
spec_schema_version: 1
id: MASK-04D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: lab2000_range
oracle_entry: lab2000_range
registered_operation_keys:
  - mask.lab2000_range_strict
---

# MASK-04D: CIEDE2000 color range

Inherits [MASK-common](MASK_common_contract.md) and the [family contract](MASK-04_color_range_contract.md).

## Interface

Registered operation key: `mask.lab2000_range_strict`.

image: Float32/Float64 planar[C,H,W], target: Float64[3] -> values: Coverage[H,W] in the image dtype. channels selects one complete CIELAB group in l,a*,b* order. Required statics: channels; input_description; kL,kC,kH positive finite Float64; inner,outer finite Float64 with 0<=inner<=outer; curve linear|smoothstep. No raw mode or implicit color conversion. Target coordinates have the same effective Lab interpretation as the image; the native l coordinate is L*/100. Constructors recommend factors 1,1,1 but serialize them. Missing or irrelevant statics are rejected.

## Mathematics

Use the CIEDE2000 equations (2)-(22) in [S26](../research-sources.md#s26). Interpret physical L* as exact 100*l without an intermediate rounding. Decimal formula constants (0.17, 0.24, 0.32, 0.20, 0.015, 0.045, and angle constants) are the exact stated rational values; pi and transcendental functions denote real mathematical values. Stored samples and parameters denote actual binary values. Use four-quadrant atan2 in degrees [0,360), with zero hue for zero adjusted chroma. If either adjusted chroma is zero, hue difference is zero and mean hue is the sum. Otherwise a difference of exactly +/-180 degrees takes the inclusive unwrapped branch; larger magnitudes wrap by 360. For the wrapped mean, sum<360 adds 360 and sum>=360 subtracts 360 before dividing by two. Signed zeros do not choose a different angular branch. Keep the rotation cross term R_T*(deltaC/(kC*S_C))*(deltaH/(kH*S_H)). No approximate epsilon replaces a branch predicate and no negative-roundoff clamping changes the real formula.

Let d be that exact nonnegative distance. If inner=outer, return [d<=inner]. Otherwise return 1 at d<=inner, +0 at d>=outer, and RN_T(1-F((d-inner)/(outer-inner))) inside. The distance is not first rounded to Float32/Float64. Identity colors have d=0. Model factors are unrelated to the statistical covariance regularization epsilon.

## Behavior

For nonempty output selection, the output depends on the selected image components and the complete target. No unrelated alpha/AOV or whole-image statistics are required. Finite consumed coordinates are caller premises; unsupported or structurally inconsistent descriptions fail preparation, and `input_description` is required even for empty output selection. Every successful response is the RN_T result of the exact expression. No commercial compatibility is implied.

## Acceptance

Decimal test data checks formula behavior only, not strict bits. Cases: zero chroma, +/-180 degree hue differences, mean-hue wrap, identical colors (d=0), finite non-unit kL,kC,kH, threshold equality and both dtypes.
