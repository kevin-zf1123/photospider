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
verification_status: focused_result_math_ctest_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-07A: apply_lut3d_trilinear

## Revised lightness coordinate and implementation boundary

The [current shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. Runtime
ColorArray v1 still encodes the old implicit L* units: public metadata, fixtures
and consumers need explicit migration before this revised target is fully aligned.
Historical implementation/test evidence below does not establish that migration;
no silent old/new unit alias or sample-magnitude inference is permitted.

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
ColorArray v1 facet and `atomic_trailing_axes=1`. Output retains the declared
ColorArray description. The default output dtype follows the authoring `input_type`
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
CRV-07 defines signed-zero and chroma legality. Generic zero-weight vertices are
numerically unused, but complete typed/upstream validation still applies.

## Execution, resources and errors

The Whole Result program requests input, table and axis with Data, Validation
and Descriptor (role 13). Callback numeric checks validate all axes, every
original input color and query before applying clamp, then perform selected
vertex mathematics. Typed/upstream errors may precede callback validation and
cover complete inputs. Requested fragments close to complete three-component
colors. Output carries ColorArray v1, records actual source ObjectIds, publishes
full certified coverage with global sample coordinates, and has one all-or-nothing
Result transaction. Dirty mapping follows recorded output demand; Empty reads no
payload. Result owners survive context teardown.

Work and resource limits, authorized Root windows, Root-owned 8*sum(Ni)-byte
grid storage, full packed output allocation, cancellation and error categories
are inherited from the [CRV-07 contract](CRV-07_apply_lut3d.md). Table samples
are accessed through authorized zero-copy Root windows; the operation does not
make a dense copy of the entire source table. Numeric failures have Run scope;
unpublished output is released on failure. Unsupported platform keys return
BackendUnavailable, and capacity/work limits fail explicitly without approximate
results.

## Workflow and acceptance

Bind a 2x2x2 RGB table whose vertex (r,g,b) stores (r*g,g*b,b*r), axis rows
[0,1,1], input=[[0.75,0.25,0.5]], matching RGB descriptions and dtype Float64.
Expect values=[[0.1875,0.125,0.375]] for this method. The public
`apply_lut3d_trilinear_node` constructor and executable workflow are linked below.

Use CRV-07's independent rational oracle, identity/affine/cross-component tests,
all supported models and mixed dtypes, grid boundaries, tetrahedral split ties
where applicable, axis directions, finite extensions, hue winding, zero signs,
partial-channel/full-color requests, zero-weight invalid rows, complete-input dirty support,
strides, budgets, cancellation and lifetime tests. Full CLF compatibility is
outside this Proposed specification.

## Maintained implementation

[`apply_lut3d_trilinear_node`](../../../../include/photospider/numeric/lut3d.hpp)
constructs this Whole Result primitive in
[`lut3d_application.cpp`](../../../../plugins/ops/01-numeric/lut3d_application.cpp).
All profiles use exact product weights and a single final rounding; only
positive-weight complete vertices participate in the mathematics. See the
[shared implementation](CRV-07_apply_lut3d.md#maintained-implementation-and-validation)
for current Result coverage, source-owner and read-window lifetime, focused
acceptance evidence, and limits on maximum-output, x86, GPU, and historical
performance claims. The maximum 256^3 table-shape check uses an 8-byte
zero-stride backing view; it does not execute a maximum-sized output.
The root manual-fixture CTest passed 1/1 in 1.48 s; a fresh installed consumer
selection passed 3/3 in 5.71 s, with the LUT3D case taking 2.36 s. Its direct
Apple run passed all six manual groups. The shared math integration timing is
reported separately in the [family implementation record](../math-implementation.md#crv-07-joint-three-dimensional-lut-application).
