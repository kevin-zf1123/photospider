---
spec_schema_version: 1
id: RES-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-04
function: cbm3d_native
proposed_operation_keys:
- restoration.cbm3d_native
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-04B: cbm3d_native

Color CBM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The fixed profile is `haar_opponent_v1`, defined by the family contract. Native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.cbm3d_native`. The C++ SDK helper `ps::restoration::cbm3d_native` appends one node transactionally.

### Ports

Inputs, in order: `input` is one nonempty rank-three Float32/Float64 image with no batch axes, two spatial axes and one component axis of extent three; `sigma` is one Float64 tensor of shape `[1]` with no batch axes. The `output` port contains a `photospider.tensor` v1 Result with one tensor named `samples`, retaining input shape, dtype, layout and component storage order. The selected RGB group is opaque and has no associated alpha. Sigma is nonnegative and describes isotropic RGB noise in input coordinate units. At sigma=0, copy input bits.

### Parameters

Supply every algorithm parameter in the [family parameter table](RES-04_contract.md#shared-parameters), plus these required static String parameters:

| Parameter | Meaning |
| --- | --- |
| `input_description` | Encoded FMT `TensorDescription`, with exactly one semantic RGB group and explicit primaries (named or chromaticities with white), transfer and scene/display reference. |
| `group` | Selected group name. Its red, green and blue roles identify all three components, and its component axis differs from both spatial axes. |

The SDK accepts `Bm3dOptions`, a `TensorDescription` and the group name, and encodes the description. Alpha, non-RGB groups and AOV components are rejected. Results carry tensor metadata; the caller retains the source color interpretation. Filtering preserves the source coordinate units and transfer encoding.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For sigma>0, map RGB samples to the fixed orthonormal opponent coordinates

$$
Y=(R+G+B)/\sqrt{3},\qquad U=(R-B)/\sqrt{2},\qquad V=(R-2G+B)/\sqrt{6}.
$$

Run the family two-stage algorithm in these coordinates. Match using Y only: `match_hard` and `match_wiener` have squared Y-coordinate units. Each stage shares selected centers across Y/U/V, while filtering and aggregation weights are computed independently per channel with the same sigma. The hard-stage basic estimate remains in opponent coordinates for Wiener matching and filtering.

Apply the transpose inverse after the Wiener stage:

$$
R=Y/\sqrt{3}+U/\sqrt{2}+V/\sqrt{6},\quad G=Y/\sqrt{3}-2V/\sqrt{6},\quad B=Y/\sqrt{3}-U/\sqrt{2}+V/\sqrt{6}.
$$

Restore components to the input role/storage order. Primaries and transfer identify the source interpretation; the operation performs this fixed opponent transform directly on supplied RGB coordinates, without transfer decoding, primary conversion, code-value scaling or clipping.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output depends on the complete plane or RGB group. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Apply the [family acceptance cases](RES-04_contract.md#behavior-and-acceptance). Check component-role permutations and component-axis positions. Verify the fixed opponent transform and transpose inverse, shared Y-based groups, independent channel weights and rejection of alpha/AOV structure.

## References

[S20 · BM3D author project](../research-sources.md#s20)
