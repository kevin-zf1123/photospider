---
spec_schema_version: 1
id: MASK-04D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-04
function: lab2000_range
oracle_entry: lab2000_range
proposed_operation_keys:
  - mask.lab2000_range_strict
---

# MASK-04D: CIEDE2000 color range

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-04_color_range_contract.md). Numerical rules follow
01-numeric. This spec does not claim native registration or production execution.

## Ports, parameters and output inference

image: Float32/Float64 planar[C,H,W], target: Float64[3] -> values: Coverage[H,W]
in the image dtype. channels selects one complete CIELAB group in l,a*,b* order.
Required statics: channels; interpretation respect|override; conditional
source_description for override; kL,kC,kH positive finite Float64; inner,outer
finite Float64 with 0<=inner<=outer; curve linear|smoothstep. No raw mode or
implicit color conversion. Target coordinates have the same effective Lab
interpretation as the image; the native l coordinate is L*/100. Constructors
recommend factors 1,1,1 but serialize them. Missing/irrelevant statics fail.

## Exact formula and numerical stages

Use the CIEDE2000 equations (2)-(22) in [S26](../research-sources.md#s26).
Interpret physical L* as exact 100*l without an intermediate rounding. Decimal
formula constants (0.17, 0.24, 0.32, 0.20, 0.015, 0.045, and angle constants)
are the exact stated rational values; pi and transcendental functions denote
real mathematical values. Stored samples and parameters denote actual binary
values. Use four-quadrant atan2 in degrees [0,360), with zero hue for zero
adjusted chroma. If either adjusted chroma is zero, hue difference is zero and
mean hue is the sum. Otherwise a difference of exactly +/-180 degrees takes
the inclusive unwrapped branch; larger magnitudes wrap by 360. For the wrapped
mean, sum<360 adds 360 and sum>=360 subtracts 360 before dividing by two.
Signed zeros do not choose a different angular branch. Keep the rotation cross
term R_T*(deltaC/(kC*S_C))*(deltaH/(kH*S_H)). No approximate epsilon replaces a
branch predicate and no negative-roundoff clamping changes the real formula.

Let d be that exact nonnegative distance. If inner=outer, return [d<=inner].
Otherwise return 1 at d<=inner, +0 at d>=outer, and
RN_T(1-F((d-inner)/(outer-inner))) inside. The distance is not first rounded to
Float32/Float64. Identity colors have d=0. Model factors are unrelated to the
statistical covariance regularization epsilon. Correctly rounded transcendental
certification may refine; resource exhaustion fails rather than guessing.

## Demand, failures and verification

For nonempty Q read selected image components at Q and the complete target.
Respect/override additionally consumes the validation closure of that color
group. No unrelated alpha/AOV or whole-image statistics. Empty Q is descriptor
only. Dirty support follows these image/target dependencies. NaN/Inf in consumed
coordinates is InvalidDomain; unsupported descriptions fail preflight. Refinement
state and retained inputs are budgeted and cancellable. No native engine adapter
or commercial compatibility is implied.

The MPFR interval oracle certifies the complete expression and exact branches;
unresolved certification reports Uncertified under its bounded manual limit.
Author decimal test data checks formula behavior only, not strict bits. Test
zero chroma, +/-180 hue differences, mean-hue wrap, identical colors, finite
non-unit factors, threshold equality, both dtypes, selected-channel demand,
invalid parameters and low budgets. Native acceptance requires a real public
workflow and host-level read/resource/lifetime checks.

Oracle entry: `lab2000_range` in [reference.py](../../../../oracle/ops/mask_morphology/reference.py).
