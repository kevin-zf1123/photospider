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
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-07B: apply_lut3d_tetrahedral

This independent primitive applies tetrahedral interpolation to a joint three-axis
color lookup table. The [complete CRV-07 contract](CRV-07_apply_lut3d.md) is normative
for all interface, formula, numerical, dependency and acceptance requirements.

## Interface and semantics

Ordered dynamic ports are input (Float32/Float64[...,3]), table
(Float32/Float64[N0,N1,N2,3]) and axis (Float64[3,3]); output is named values,
with input shape and selected floating dtype, default input dtype. Source float
dtypes may differ. Input rank is 2..8, logical count <=2^40; each table extent
is 2..256, independently. Each axis row is [start,end,step], with globally
validated ascending/descending reconstructed Float64 coordinates.

Required static String parameters are input_color_description,
output_color_description, dtype and out_of_domain (reject/clamp, default reject).
Input/output models must match among RGB, XYZ, CIELAB, OKLab, CIELCh(ab), OKLCh,
HSL and YCbCr; descriptions may differ within the chosen model. No alpha or
four-channel CMYK input is supported. The table's first three axes follow the
input model channel order and its last axis the output model channel order.
Lookup uses actual values; no transfer, model conversion or hue normalization
is performed. Output carries the declared output color description.

Use the fixed main-diagonal six-tetrahedra subdivision. Sort exact local coordinates descending, tie by axis 0,1,2, then use its four cumulative-axis vertices.
Read only vertices with exact nonzero weights, at most 4 complete colors.
Weights and complete weighted sums are exact before one final dtype rounding.
All three CPU versions are bitwise identical. Single-vertex and all-negative-zero
mixture rules follow CRV-07; zero-weight vertices never affect result or failure.

## Execution, resources and errors

A request for any component observes the full color. Inherit global axis
validation, requested complete input reads, exact contributing table support,
typed/upstream closures, dirty witnesses and descriptor-aware cache identity.
Results retain immutable backing/metadata beyond context lifetime and support
arbitrary legal source strides. Return packed fragments at global request origins.

Work is axis validation plus per-color lookup and at most 4 vertices of exact
arithmetic. Budget output, read owners/windows, exact limbs, lookup maps and growth
overlap as specified by CRV-07. No whole table copy is required. Host budgets,
bounded cancellation, cache-off and atomic publication requirements are mandatory.
Malformed statics, type/description mismatch, invalid demanded colors/grid/domain,
overflow, resources and unavailable platform keys use the exact CRV-07 error
phases/categories; failed colors publish no partial channels.

## Conceptual workflow and acceptance

Bind a 2x2x2 RGB table whose vertex (r,g,b) stores (r*g,g*b,b*r), axis rows
[0,1,1], input=[[0.75,0.25,0.5]], matching RGB descriptions and dtype Float64.
Expect values=[[0.25,0.25,0.5]] for this method. This conceptual workflow requires
future public Compiler/ExecutionContext registration and actual run evidence.

Use CRV-07's independent rational oracle, identity/affine/cross-component tests,
all supported models and mixed dtypes, grid boundaries, tetrahedral split ties
where applicable, axis directions, finite extensions, hue winding, zero signs,
partial-channel/full-color requests, zero-weight invalid rows, exact dirty support,
strides, budgets, cancellation and lifetime tests. No runtime or full CLF
compatibility is claimed by this Proposed specification.
