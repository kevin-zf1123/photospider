---
spec_schema_version: 1
id: CRV-07A
parent_id: CRV-07
function: apply_lut3d_trilinear
operation_family: curve.apply_lut3d_trilinear
proposed_operation_keys:
  - curve.apply_lut3d_trilinear_strict
  - curve.apply_lut3d_trilinear_accelerated_apple_silicon
  - curve.apply_lut3d_trilinear_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-07A: apply_lut3d_trilinear

## Revised lightness coordinate and implementation boundary

The [current shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. The current runtime ColorArray v1 encodes L* in its implicit 0..100 unit;
this target scale applies only after public metadata, fixtures and consumers
adopt it explicitly. No implicit unit alias or inference from sample magnitude
is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

This independent primitive applies trilinear interpolation to a joint three-axis
color lookup table. The [complete CRV-07 contract](CRV-07_apply_lut3d.md) is normative
for all interface, formula, numerical, dependency and acceptance requirements.

## Interface and semantics

Ordered dynamic ports are Result inputs: input, table and axis. Each Result has
one tensor member under any schema id/version and member key; shapes use complete
`sample_shape()` including batch axes. Input is Float32/Float64 [...,3], rank 2..8
and logical product <=2^40. Table is Float32/Float64 [N0,N1,N2,3], each Ni=2..256;
axis is Float64 [3,3], with rows [start,end,step]. Input/table dtypes may differ.
Output port `values` is an immutable packed Result using `photospider.tensor`
v1/member `samples`, with the complete input shape, selected Float32/Float64 dtype,
`atomic_trailing_axes=1`. Output carries no ColorArray description; its descriptor contains the full input sample shape without preserving the input batch-axis partition. The default output dtype follows the authoring `input_type`
hint; the compiler independently validates bound input metadata.

Required static parameters are String `input_color_description`,
`output_color_description`, `dtype` and `out_of_domain` (reject/clamp, default
reject). Input/output descriptions use the same supported three-component model;
descriptions may differ within that model. No alpha or four-channel CMYK input is
supported. Table grid axes follow input model channel order and its value component
axis follows the output model channel order. Lookup uses actual coordinates; no
transfer conversion, model conversion or hue normalization is performed.

The method uses the product of three linear weights over the eight cell vertices.
For each vertex bit vector b in {0,1}^3, its weight is the product of `t_i`
when `b_i=1` and `1-t_i` otherwise. Mathematically use only exact positive-weight
vertices, at most 8 complete colors. Weights and complete weighted sums
are exact before one final destination-type rounding. Strict is correctly
rounded; accelerated profiles follow the shared FP32-scaled final-result bound.
CRV-07 defines signed-zero and chroma legality. Generic zero-weight vertices are numerically unused. Upstream failures still propagate through the complete-table dependency closure.

## Execution, resources and errors

Each selected output tile emits one fixed CPU task using invocation ABI 8. Reads cover that tile's query region, the complete table, and axis endpoints [0:3,0:2], closed on each source's own grid. Preparation validates the fixed signature. Numerical axis, model, domain and finite-output conditions are unchecked caller preconditions, as specified by CRV-07.

The kernel writes preallocated dense complete-color tiles with global coordinates and owns fixed arithmetic/knots scratch for the invocation. Empty requests emit no tasks. The executor retires the call's selected Results after successful task completion; retained owners survive context teardown. Static/resource failures and cancellation remain operational errors. The [CRV-07 contract](CRV-07_apply_lut3d.md#resource-and-failure-contract) defines scratch, dependency and lifetime requirements.

## Workflow and acceptance

Bind a 2x2x2 RGB table whose vertex (r,g,b) stores (r*g,g*b,b*r), axis rows
[0,1,1], input=[[0.75,0.25,0.5]], matching RGB descriptions and dtype Float64.
Expect values=[[0.1875,0.125,0.375]] for this method. The public
`apply_lut3d_trilinear_node` constructor and executable workflow are linked below.

Use CRV-07's independent rational oracle, identity/affine/cross-component tests,
all supported models and mixed dtypes, grid boundaries, tetrahedral split ties
where applicable, axis directions, finite extensions, hue winding, zero signs,
complete-color tile requests, zero-weight invalid rows, source-grid dependency closure,
strides, budgets, cancellation and lifetime tests. Full CLF compatibility is
outside this Proposed specification.

## Maintained implementation

[`apply_lut3d_trilinear_node`](../../../../plugins/ops/include/photospider/ops/numeric/lut3d.hpp)
constructs this fixed Result primitive in
[`lut3d_application.cpp`](../../../../plugins/ops/01-numeric/lut3d_application.cpp).
Only positive-weight complete vertices enter exact arithmetic. See the [shared implementation](CRV-07_apply_lut3d.md#maintained-implementation-and-validation) and [mathematical implementation](../math-implementation.md#crv-07-joint-three-dimensional-lut-application) for current execution and independent acceptance requirements.
