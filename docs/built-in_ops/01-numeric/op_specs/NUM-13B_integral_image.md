---
spec_schema_version: 1
id: NUM-13B
parent_id: NUM-13
function: integral_image
proposed_operation_keys:
  - numeric.integral_image_strict
  - numeric.integral_image_accelerated_apple_silicon
  - numeric.integral_image_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-13B: integral_image

Two-axis summed-area table with zero boundaries. Inherits the [NUM baseline](NUM_common_contract.md), overridden where unchecked numerical preconditions differ.

## Interface

The three `numeric.integral_image_<profile>` keys accept one Result tensor member under any member key. Output port `values` uses `photospider.tensor` with member `samples`. The output takes the complete input `sample_shape()`, including batch axes, as ordinary axes and adds one to each selected extent. Other coordinates identify independent planes. No image/color semantics or physical units are inferred.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `axes` | String naming exactly two distinct nonnegative input axes as canonical comma-separated integers; normalized to increasing order | Required |
| `dtype` | String following [prefix_sum](NUM-13A_prefix_sum.md)'s same-domain rules | Helper: Int64 for integer inputs, Float64 for floating inputs |

Input rank is 2..8 with positive extents; both input and output logical counts are at most 2^40. Static schema, shape caps, axes and dtype errors fail before execution. Public helpers are in `photospider/ops/numeric/scans.hpp`.

## Mathematics

For selected axes a,b and output coordinate o, the value sums source coordinates s[a] in [0,o[a]) and s[b] in [0,o[b]), with other coordinates equal to o. Each result is an exact rectangle sum followed by one conversion. The arithmetic and unchecked integer precondition follow [prefix_sum](NUM-13A_prefix_sum.md). A rectangle with either boundary zero yields integer zero or floating +0. Source NaN priority follows original logical row-major coordinates, independent of physical strides and tile layout; the sum contract's payload conversion, mixed-infinity and zero-sign rules apply. Rounded output and generated NaN never become source carry. Floating overflow remains an output infinity while later rectangles keep exact cancellation.

Four-corner subtraction of rounded floating outputs has its own rounding and cancellation error; each integral value is guaranteed independently.

## Behavior

The output at o depends on the source rectangle from zero to o on the selected axes, in the same plane. A boundary-zero row or column has no source dependency. Empty selection reads nothing.

## Acceptance

Input `[[1,2],[3,4]]`, axes `"0,1"` gives `[[0,0,0],[0,1,3],[0,4,10]]`. Cases: nonadjacent axes, independent planes, NaN priority, zero-only unions, sparse requests, against independent exact rectangle enumeration.
