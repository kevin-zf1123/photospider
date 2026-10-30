---
spec_schema_version: 1
id: CRV-07B
parent_id: CRV-07
function: apply_lut3d_tetrahedral
operation_family: curve.apply_lut3d_tetrahedral
proposed_operation_keys:
  - curve.apply_lut3d_tetrahedral_strict
  - curve.apply_lut3d_tetrahedral_accelerated_apple_silicon
  - curve.apply_lut3d_tetrahedral_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-07B: apply_lut3d_tetrahedral

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

This independent primitive applies tetrahedral interpolation to a joint three-axis color lookup table. The [complete CRV-07 contract](CRV-07_apply_lut3d.md) is normative for all interface, formula, numerical, dependency and acceptance requirements. Color configuration uses canonical TensorDescription v4/v5 tuple parameters, with native CIELAB/CIELCh `l=L*/100` per the [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md).

## Interface

Ordered dynamic ports are Result inputs: input, table and axis. Each Result has one tensor member under any schema id/version and member key; shapes use complete `sample_shape()` including batch axes. Input is Float32/Float64 [...,3], rank 2..8 and logical product <=2^40. Table is Float32/Float64 [N0,N1,N2,3], each Ni=2..256; axis is Float64 [3,3], with rows [start,end,step]. Input/table dtypes may differ. Output port `values` is an immutable packed Result using `photospider.tensor` v1/member `samples`, with the complete input shape, selected Float32/Float64 dtype, `atomic_trailing_axes=1`. Output carries no color description; its descriptor contains the full input sample shape without preserving the input batch-axis partition. The default output dtype follows the authoring `input_type` hint.

Required static parameters are String `input_color_description`, `output_color_description`, `dtype` and `out_of_domain` (reject/clamp, default reject). Both descriptions use `tensor_description_parameter` and canonical logical tuples (`channel_axis=0`) with floating hue only. Input/output descriptions use the same supported three-component model; descriptions may differ within that model. No alpha or four-channel CMYK input is supported. Table grid axes follow input model channel order and its value component axis follows the output model channel order. Lookup uses actual coordinates; no transfer conversion, model conversion or hue normalization is performed. The public constructor is `apply_lut3d_tetrahedral_node`.

## Mathematics

The method uses the fixed main-diagonal six-tetrahedra subdivision. Exact local coordinates are sorted descending, breaking ties by input axis 0,1,2, and the four cumulative-axis vertices are used. The tetrahedral vertices are `000`, `e_a`, `e_a+e_b`, `111`; weights are `1-t_a`, `t_a-t_b`, `t_b-t_c`, `t_c`, respectively. Only exact positive-weight vertices (at most 4 complete colors) participate. Weights and complete weighted sums are exact before one final destination-type rounding. Strict is correctly rounded; accelerated profiles follow the shared FP32-scaled final-result bound. CRV-07 defines signed-zero and chroma legality.

## Behavior

Each output color depends on its query color, the axis endpoints [0:3,0:2] and its positive-weight table vertices; generic zero-weight vertices are numerically unused. Numerical axis, model, domain and finite-output conditions are unchecked caller preconditions, as specified by CRV-07. Region and tile results equal the corresponding slice of the whole result.

## Acceptance

A 2x2x2 RGB table whose vertex (r,g,b) stores (r*g,g*b,b*r), axis rows [0,1,1], input=[[0.75,0.25,0.5]], matching RGB descriptions and dtype Float64 gives values=[[0.25,0.25,0.5]] for this method.

The CRV-07 exact rational reference applies: identity/affine/cross-component tests, all supported models and mixed dtypes, grid boundaries, tetrahedral split ties where applicable, axis directions, finite extensions, hue winding, zero signs and zero-weight invalid rows. Full CLF compatibility is outside this specification.
