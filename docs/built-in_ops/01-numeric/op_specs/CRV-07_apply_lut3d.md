---
spec_schema_version: 1
id: CRV-07
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
verification_status: focused_result_math_ctest_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-07: three-dimensional LUT application

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

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/
registration status, CPU target identity, floating environment, diagnostic provenance,
host observation isolation, resource accounting and public/performance acceptance.
In particular, rounding is nearest/ties-to-even with gradual underflow and the
caller floating environment is preserved. This is limited inheritance: the ports,
output kinds/facets, observation units, mathematical rounding boundaries and
explicit numerical/error rules in this specification take precedence. It does not
turn a composite template or structured Result into a generic NUM Value primitive.


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

Dynamic axis is Float64[3,3]. Rows correspond to input components 0,1,2 and each contains
[start,end,step]. Reconstruct each axis using its table extent N0,N1,N2 and
the exact Float64 coordinate and step-consistency rules of CRV-05. Globally
validate every reconstructed coordinate on all three axes for strict order;
each axis independently supports increasing or decreasing coordinates.
Each axis extent is 2..256; unequal N0,N1,N2 are supported. Singleton axes are
not accepted. The largest table has 256^3*3 values, 384 MiB at Float64. A Whole
request validates the complete table with typed/upstream support; sample access
uses authorized Root windows rather than requiring a dense table copy.

## Confirmed color spaces

Declare static input and output color descriptions, with identical model identity.
Input metadata matches the input description; table component values match the
output description, and the result carries it. Descriptions may differ within
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
authoring `input_type` hint, independently of the bound Result dtype. All demanded color
components and final outputs must be finite and obey the relevant model's
legality rules. In particular CIELCh/OKLCh require C>=0; HSL S/L and other signed/
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

Each input is a Result containing one tensor member under any schema id/version
and member key. Use its complete `sample_shape()`: input [...,3] rank 2..8,
table [N0,N1,N2,3], and axis [3,3]. The Whole Result program requests all three
with Data, Validation and Descriptor (role 13), including full typed validation
and upstream failures. Typed/upstream failures can precede callback numeric
checks. Callback validation checks all axes first, then every original input
color/query before clamp, applies clamp to valid original coordinates, then runs
selected-vertex table arithmetic. Only exact positive-weight vertices enter the numerical
formula: at most eight for trilinear or four for tetrahedral. Generic invalid
zero-weight vertices remain mathematically unused; typed invalid data or an
upstream failure there still fails the run. Each contributing vertex is a
complete three-component color.

The output port `values` is a Result using `photospider.tensor` v1/member
`samples`, with the complete input shape, selected Float32/Float64 dtype, the
ColorArray v1 facet and `atomic_trailing_axes=1`. It publishes a packed immutable
output with full certified coverage and global sample coordinates. The output
association records actual source ObjectIds. Any input edit invalidates the
recorded output demand; dirty support follows that demand. Empty reads no payload.
Each key has one all-or-nothing Result transaction; numeric failure scope is Run.

Input `sample_shape()` has rank 2..8, last extent 3 and logical element count
<=2^40. A single color uses [1,3]. The observation domain is the remaining input
axes, rank 1..7.

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
interpolation or implicit periodic domain. Input/output model constraints are
checked before/after these formulas, including required chroma nonnegativity.

## Parameters, demand and output mapping

Ordered dynamic inputs are input, table, axis; output is values. Required static
String parameters are input_color_description, output_color_description, dtype,
out_of_domain. Constructors provide the documented dtype/policy defaults;
descriptions explicitly identify the same supported model, no alpha, applicable
white/primaries/transfer and hue/coordinate units. Generic arrays may be interpreted
by these declarations; existing typed color descriptions must match rather than
be silently replaced. Table has the output description. No extra interpolation
mode is accepted because the methods are independent operations.

Empty Q has no payload reads; static Result schemas and descriptions are still
preflight validated. Nonempty requests request complete inputs with role 13 before
the callback. Typed/upstream errors can therefore precede callback numeric
validation. Among callback numeric checks, axis validation precedes every
original input color check, input validation precedes clamp, and all query/domain
checks precede table arithmetic. No per-output dependency or point arrays remain.

Any input edit invalidates the complete recorded output demand; dirty support
follows that demand. Cache identity retains method, descriptions, grid, dtype,
policy and complete typed/upstream support. The immutable packed Result carries
ColorArray v1 metadata and marks the trailing extent-3 component axis atomic with
`atomic_trailing_axes=1`.
Local requests still return full certified coverage in global sample coordinates.
Legal unaligned, offset, negative/zero-stride input/table layouts remain accepted,
and output owners survive context teardown.

## Resource and failure contract

For M=product(input.sample_shape())/3 colors, grid validation is O(N0+N1+N2),
lookup O(M*sum(log Ni)), and arithmetic uses at most 8 or 4 contributing
vertices per color. Output payload is 3*M*sizeof(dtype). `UniformAxis` stores
Root-owned Float64 grids costing 8*(N0+N1+N2) bytes plus allocator/metadata
overhead. Budget exact workspace, authorized source windows/retained owners, one
current vertex/weight set, full packed output and descriptors. Input/table access
uses zero-copy Root windows; no dense copy of the full source table is required.
The 384 MiB maximum table payload is logical size and does not guarantee that all
active windows and output fit a particular Root budget.

Inherit CRV-06A's bounded cancellation polling and host capacity/work
obligations, with polls in axis scans, lookup batches and exact-arithmetic work.
Release temporary state after success/failure/cancellation and retain only owned
result state. Unsupported platform keys fail BackendUnavailable; resource limits
fail ResourceExhausted, never return an approximate interpolation.

Malformed static descriptions/parameters or extents outside supported ranges
fail compile/preflight with InvalidArgument/InvalidDomain; shape/dtype or attached
description mismatch uses TypeMismatch. Invalid axes (including derived overflow,
duplicate coordinates or step mismatch), nonfinite/model-invalid demanded colors
and reject-domain violations fail OperationFailed/InvalidDomain. Final component
narrowing overflow fails OperationFailed/ArithmeticOverflow. Preserve upstream,
typed, stale, cancellation and resource categories and provenance. Numeric failure scope is Run; no partial output survives a failed callback.

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

Verify partial channel requests expand to full colors, zero-weight invalid
generic vertices remain mathematically unused, typed validation covers all inputs, invalid demanded chroma fails even if domain-clamp would
hide it, axis errors are global, dirty support covers complete inputs, and all output metadata
matches the declared model/space. Include strides, owners after context teardown,
low budgets, cancellation, cache-off and joint versus partitioned requests.

The public workflow binds input/table/axis, explicitly supplies both
descriptions/dtype/policy, evaluates values through Compiler/ExecutionContext,
and inspects complete colors and output metadata. Actual run commands and
independently checked expected values are linked in the implementation section.
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

The six operation keys are registered in
[`lut3d_application.cpp`](../../../../plugins/ops/01-numeric/lut3d_application.cpp),
and [`lut3d.hpp`](../../../../include/photospider/numeric/lut3d.hpp) provides the
public trilinear and tetrahedral constructors. Both are Whole Result operations.
Static preparation validates Result tensor schemas and ColorArray facets, then
stores reusable immutable program state. Each continuation records current
source ObjectIds and uses authorized Root read windows for input/table samples.
It publishes complete global-coordinate coverage atomically; escaped Results
and read windows retain storage after context retirement. `UniformAxis` owns the
Root grids, while output and exact-arithmetic workspace use managed Root
resources.

The `lut3d.cpp` manual Result workflow passes six groups under Strict and
Apple. The independent Fraction oracle passes 1,062 cases per profile with its
existing expected-string gate unchanged. The registered
`test_numeric_lut3d_result` runs this manual fixture under Strict;
`test_numeric_result_math` exercises separate LUT workflow, boundary,
preparation, and resource fixtures. The installed consumer compiles the same
public workflow source against the installed 0.32.0 `Photospider::kernel`
package; commands and direct coverage are in the
[public workflow section](../../../../examples/numeric_workflow/README.md#joint-three-axis-color-luts).
The root LUT3D-specific CTest passed 1/1 in 1.48 s (1.49 s total). The shared
math test passed in a separate CTest selection that also included baking
coverage (4.83 s). The fresh installed-consumer selection passed 3/3 in 5.71 s,
with `installed_numeric_lut3d_result` taking 2.36 s; its direct Apple run
passed all six groups. These timings distinguish the LUT3D-specific test from
shared integration coverage.

The maximum 256^3 Float64 table shape succeeds with an 8-byte backing read
window under an 8 MiB Root Payload cap, proving that the implementation does
not make a dense table copy. A separate sparse request requiring 2^38 output
positions is rejected as `CapacityLimit` at node 1 because the complete output
exceeds the cap; that case does not establish numerical execution at this size.
These checks do not establish x86 or native GPU execution. The 0.18.0 benchmark
data in the math implementation is historical Value-path evidence and does not
measure the Result implementation. Runtime ColorArray v1 still encodes the
earlier implicit CIELAB lightness units; the revised `l=L*/100` contract remains
a known gap, and no silent conversion is performed. Specification acceptance
remains Proposed.
