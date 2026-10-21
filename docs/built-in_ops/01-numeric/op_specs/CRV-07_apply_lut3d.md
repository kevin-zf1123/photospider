---
spec_schema_version: 1
id: CRV-07
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-07: three-dimensional LUT application

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output
table coordinates and validation points. Finite values outside 0..1 remain
legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged.
The implementation uses canonical TensorDescription v4/v5 tuple parameters.
The old ColorArray types and decoder are removed; old payloads and explicit
L* units are rejected without sample-magnitude inference.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/
registration status, CPU target identity, floating environment, diagnostic provenance,
host observation isolation, resource accounting and public/performance acceptance.
In particular, rounding is nearest/ties-to-even with gradual underflow and the
caller floating environment is preserved. This is limited inheritance: the ports,
output structures, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Tensor primitive.

## Discrete table and explicit interpolation selection

The table samples a finite grid; it is not a mapping entry for each representable
floating input. For example a 33x33x33 grid stores 35,937 three-component colors,
and values between grid vertices are interpolated. The floating input dtype does
not determine the number of table entries or imply a 2^32-entry allocation.

Interpolation is explicitly selected by operation identity:
apply_lut3d_trilinear or apply_lut3d_tetrahedral, each with its three CPU versions.
There is no implicit interpolation default or additional method parameter. The
choice matters: the two methods can produce different values from the same grid,
as demonstrated by the cross-component fixture below. Nr/N0 and the other table
extents control sampling density; they do not select the interpolation algorithm.

The maintainer selected independent trilinear and tetrahedral interpolation
operations for three-component color arrays. Supported models are RGB, XYZ,
CIELAB, OKLab, CIELCh(ab), OKLCh, HSL and YCbCr. Input and output must use the
same model. Input is [...,3], table is [N0,N1,N2,3], and output values has the
complete input sample shape.
The first three table axes follow the input model's channel order; the last axis
contains output model components. This is a joint three-variable mapping, not
three independent one-dimensional LUTs or a scalar color ramp.

Channel order is RGB; X,Y,Z; l,a*,b*; L,a,b; l,C*,h; L,C,h; H,S,L; or Y',Cb,Cr,
respectively. Hue uses the original floating radian/pi-multiple value without
normalization. No implicit periodic lookup or hue endpoint substitution occurs.
CMYK's four input dimensions and alpha are separate from this initial scope.

## Confirmed grid coordinates

Dynamic axis is Float64[3,3]. Each row is [start,end,step] for one input component. Each extent Ni is 2..256, independently ascending or descending. The caller guarantees finite endpoints and step, distinct endpoints, and step bits equal to RN64((end-start)/(Ni-1)), with a finite nonzero result of the correct direction. Interior knots are RN64(((Ni-1-i)*start+i*end)/(Ni-1)); the first and last knots preserve endpoint bits. Every reconstructed knot must be strictly ordered numerically, so -0 and +0 cannot form separate knots.

These numerical conditions are unchecked caller preconditions. The fixed kernel reconstructs knots from the six endpoints. The redundant step column does not contribute to interpolation. Unequal extents are supported; the largest table has 256^3*3 values, or 384 MiB at Float64.

## Confirmed color spaces

Declare static input and output color descriptions, with identical model identity.
Input coordinates are interpreted using the input description; table and output
coordinates use the output description. Results carry no description. Descriptions may differ within
the same model: e.g. RGB primaries/transfer, CIELAB white, or polar hue unit.
The LUT values express that transformation; OKLab/OKLCh still require fixed D65.
Lookup uses the actual input component values directly. No implicit decoding,
white adaptation, model conversion or re-encoding surrounds interpolation.

## Confirmed boundary behavior

Static out_of_domain is reject or clamp, default reject. Clamp each input
component independently to that axis's actual numeric minimum/maximum, then
apply the selected interpolation method. Descending axes use numeric endpoints,
not an assumption that the first stored coordinate is smaller. No extrapolation
is supported in this initial contract.

## Confirmed types and model validity

input and table independently accept Float32/Float64, axis is Float64, and static
output dtype is Float32/Float64; the public helper defaults it from the explicit
authoring `input_type` hint, independently of the bound Result dtype. Consumed query components, positive-weight table colors and final outputs must be finite and obey the relevant model's legality rules as unchecked caller preconditions. Original query colors must be legal before clamp; complete positive-weight table colors must be legal in the output description. In particular CIELCh/OKLCh require C>=0; HSL S/L and other signed/
HDR coordinates retain the finite extensions selected in CRV-06. No automatic
hue normalization or nominal-range clipping occurs.

## Confirmed precision

Both methods provide strict, accelerated_apple_silicon and accelerated_x86_64
operation keys. Strict correctly rounds the complete interpolation reference
formula; accelerated final results use the shared FP32-scaled bound. The strict
reference computes exact weights from the reconstructed
Float64 grid coordinates and actual input values, then correctly round each
whole weighted sum once at output dtype. Do not use rounded per-axis blends as
the reference mathematical function.

## Confirmed table demand

Required static `input_color_description` and `output_color_description` Strings
use `tensor_description_parameter` on canonical logical color tuples. Both
require three components, no alpha and a floating hue unit; rational hue is
restricted to split-port ramps. They define the same-model input and
table/output interpretation explicitly; no description is read from or attached
to a Result. `dtype` and `out_of_domain` remain explicit parameters.

Each input is a Result containing one tensor member under any schema id/version and member key. Shapes use complete `sample_shape()`: input [...,3] has rank 2..8 and logical element count <=2^40, table is [N0,N1,N2,3], and axis is [3,3]. A single query color uses [1,3]. Static preparation validates schemas, dtypes, extents and both descriptions.

The output `values` uses a fresh `photospider.tensor` v1 schema with member `samples`, the complete input sample shape as its descriptor shape, selected Float32/Float64 dtype, and `atomic_trailing_axes=1`. It carries no color description and does not preserve the input's batch-axis partition. Each selected output tile therefore contains complete three-component colors.

Lowering declares the query region corresponding to that output tile, the complete table, and axis region [0:3,0:2]. Each region closes to complete tiles on that input's own grid; a granted source tile may contain unused samples or the step column. Table closure is conservative because vertex addresses depend on runtime queries. Upstream failures in required table tiles propagate even when a vertex would have zero weight. The LUT kernel itself reads only exact positive-weight complete vertices, at most eight for trilinear and four for tetrahedral. Numerical conditions on unused table vertices are outside this consumer's preconditions.

## Interpolation formulas

For each input axis, choose adjacent table indices j,j+1 containing the query
after any clamp. At an interior exact grid coordinate choose the cell starting
at that index; at the terminal coordinate choose the last cell. This rule uses
index order, including descending coordinate arrays. Local coordinates are exact
t_i=(query_i-axis_i[j_i])/(axis_i[j_i+1]-axis_i[j_i]), in [0,1].

Trilinear: for vertex bits b in {0,1}^3, weight is the product of t_i when b_i=1
and 1-t_i otherwise. Each component is the correctly rounded exact sum of
weight*table[j+b,component]. Only exact positive weights contribute.

Tetrahedral: split every unit cube along (0,0,0) to (1,1,1) into six tetrahedra.
Sort t descending, breaking ties by input axis index 0,1,2, to obtain a,b,c.
Vertices are v0=000, v1=e_a, v2=e_a+e_b, v3=111. Weights are 1-t_a,
t_a-t_b, t_b-t_c, t_c. Correctly round the exact weighted component sum once.
Skip zero weights. All comparisons and differences are exact rational operations,
so cell/split classification never depends on floating evaluation order.

Both formulas are convex within the cell. A table's hue components are ordinary
unnormalized numeric values in their declared output unit; there is no circular
interpolation or implicit periodic domain. Input/output model constraints, including chroma nonnegativity, are caller preconditions on original queries, contributing table colors and final outputs.

## Parameters, demand and output mapping

Ordered dynamic inputs are input, table, axis; output is values. Required static
String parameters are input_color_description, output_color_description, dtype,
out_of_domain. Constructors provide the documented dtype/policy defaults;
descriptions explicitly identify the same supported model, no alpha, applicable
white/primaries/transfer and hue/coordinate units. Generic arrays may be interpreted
by these declarations; table values use the output description. No extra interpolation
mode is accepted because the methods are independent operations.

Empty Q emits no tasks and reads no payload; static schemas and descriptions are still validated. Every nonempty selected output tile has one fixed CPU invocation. The Plan fixes its input slots, dense output allocation and scratch size. Preparation checks the kernel symbol/contract, exact output schema, read closure, output region, dtypes, Host placement and scratch requirements. Affine source views may be offset, unaligned, negative-stride or zero-stride when their address ranges are valid.

The kernel borrows its input views and exclusively writes its selected output tile. It does not allocate a complete output for unselected positions. The executor publishes the call's selected Results after all required tasks succeed in retirement order; retained Result owners survive context teardown. Immutable preparation can be shared across Runs, while each invocation owns its mutable scratch.

## Resource and failure contract

For P query colors in an output tile, knot reconstruction costs O(N0+N1+N2), lookup O(P*sum(log Ni)), and exact arithmetic uses at most eight or four vertices per color. Each invocation reserves fixed scratch containing 3*256 Float64 knot slots, ExactSampling, a 96-slot polynomial arena of 40960-bit integers, and current cell/query/color state. Scratch size is independent of query count and grid extents. Only selected output tiles are allocated. The kernel borrows source tiles without copying the complete table; Plan metadata, retained source allocations and any declared input COPY tasks have separate costs. Lowering bounds the required read-tile count before enumeration.

Cancellation is polled during knot reconstruction, query processing and exact arithmetic. Mutable scratch is destroyed at synchronous return, and the caller's floating environment is restored. The arithmetic borrows a synchronous work callback and allocates no heap storage during invocation. Unsupported CPU profiles fail BackendUnavailable; structural and resource failures retain their operational status. Insufficient capacity never substitutes approximate interpolation.

Malformed descriptions/parameters or extents outside supported ranges fail static preparation with InvalidArgument; incompatible shape/dtype/schema uses TypeMismatch. The caller guarantees axis consistency/order, finite and model-valid original queries and contributing table colors, the closed axis domain under reject, and finite final values in the chosen output dtype. Clamp acts on otherwise valid original queries. Violating these numerical preconditions has no promised domain or overflow diagnostic. Cancellation, stale-plan, upstream, allocation and resource failures remain operational failures; failed tasks do not publish successful call outputs.

## Independent acceptance and public workflow

Use exact-rational vertex weights and component sums plus independent correctly
rounded dtype conversion. Identity tables, affine transforms, grid vertices,
faces/edges, split-plane ties, all six tetrahedra, all axis directions, unequal
axis extents, mixed dtypes and each supported model/description pair are required.
Use finite signed/HDR values and large opposite values to expose premature
rounding/overflow. Strict matches the method reference bitwise; accelerated uses the shared
FP32-scaled final-result bound;
trilinear and tetrahedral are not required to match each other.

For axis rows [0,1,1] and a 2x2x2 table at binary vertices (r,g,b), store
(r*g,g*b,b*r). Query [0.75,0.25,0.5] gives trilinear
[0.1875,0.125,0.375] and tetrahedral [0.25,0.25,0.5]. Use an RGB description
for this fixture. A polar-model table with hue 0 to 4 retains intermediate hue 2.
No modulo reduction can be substituted for that fixture.

Verify that output tiles preserve complete colors, that generic zero-weight vertices remain numerically unused, and that dependency closure includes all table tiles and only required query/axis tiles on their respective grids. Use valid original colors under clamp; invalid numerical inputs do not have a runtime-error oracle. Check output schema, arbitrary source strides, source immutability, floating-environment restoration, cancellation, bounded scratch and retained Result lifetime.

The public workflow binds input/table/axis, explicitly supplies both
descriptions/dtype/policy, evaluates values through Compiler/ExecutionContext,
and inspects complete colors and output metadata. The implementation section links the independent oracle used for expected values.
The [CLF v3 interpolation appendix](https://docs.acescentral.com/clf/specification/#appendix-a-interpolation)
is a method reference, not a claim of full CLF file, domain or bitwise compatibility.

## Signed zero

If exactly one vertex contributes, its weight is one: directly convert its color
and preserve zero signs. With multiple contributing vertices, a component's
exact zero is -0 only when every contributing component is -0; otherwise it is
+0. Nonzero underflow preserves the mathematical result's sign. Zero-weight
vertices never participate in sign determination. These rules apply equally to
hue components and every supported color model.

- [Trilinear primitive](CRV-07A_apply_lut3d_trilinear.md).
- [Tetrahedral primitive](CRV-07B_apply_lut3d_tetrahedral.md).

- [Curve category](../curves.md).
- [1D LUT contract](CRV-05_apply_lut1d.md).
- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator template](../../00-foundation/spec-template.md).

## Maintained implementation and validation

The six operation keys are registered in [`lut3d_application.cpp`](../../../../plugins/ops/01-numeric/lut3d_application.cpp), and [`lut3d.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/lut3d.hpp) provides the public constructors. They lower to `lut3d` / `lut3d-fixed-v2` kernels using invocation ABI 8. All profiles currently use exact integer weights and a single final rounding. [`ExactLut3d`](../../../../plugins/ops/01-numeric/exact_lut3d.hpp) uses borrowed callbacks and invocation-owned arithmetic state.

The independent [`CRV-07 oracle`](../../../../oracle/ops/numeric/crv07/reference.py) reconstructs RN64 grids and exact rational cells/weights, then rounds sums with separate IEEE conversion logic. Temporary acceptance should compare valid fixtures through the public Compiler/ExecutionContext path and direct prepared kernels, including sparse requests, different input grids, concurrent Runs, cancellation, strides, allocation counts and owner lifetime. Error fixtures for violated numerical preconditions are not fixed-executor acceptance cases.

Native `l=L*/100`, the public TensorDescription tuple API and independent fixtures
are migrated together. See [migration evidence](../NUM-CRV_color_migration.md).
These CPU implementations do not establish native GPU behavior.

Specification acceptance remains Proposed.
