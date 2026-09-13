---
spec_schema_version: 1
id: CRV-07
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-07: three-dimensional LUT application

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
operations for three-component color arrays. Supported models are RGB, XYZ, CIELAB,
OKLab, CIELCh(ab), OKLCh, HSL and YCbCr. Input and output must use the same
model. Input is [...,3], table is [N0,N1,N2,3], and values has input shape.
The first three table axes follow the input model's channel order; the last axis
contains output model components. This is a joint three-variable mapping, not
three independent one-dimensional LUTs or a scalar color ramp.

Channel order is RGB; X,Y,Z; L*,a*,b*; L,a,b; L*,C*,h; L,C,h; H,S,L; or Y',Cb,Cr,
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
not accepted. The largest table has 256^3*3 values, 384 MiB at Float64, although
only demanded table values need to be read and retained under actual host budgets.

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
output dtype is Float32/Float64 defaulting to input dtype. All demanded color
components and final outputs must be finite and obey the relevant model's
legality rules. In particular CIELCh/OKLCh require C>=0; HSL S/L and other signed/
HDR coordinates retain the finite extensions selected in CRV-06. No automatic
hue normalization or nominal-range clipping occurs.

## Confirmed precision

Both methods provide strict, accelerated_apple_silicon and accelerated_x86_64
operation keys. All three versions correctly round the complete interpolation
formula and are bitwise identical. Compute exact weights from the reconstructed
Float64 grid coordinates and actual input values, then correctly round each
whole weighted sum once at output dtype. Do not use rounded per-axis blends as
the reference mathematical function.

## Confirmed table demand

Only vertices with exact nonzero interpolation weight are Data/Validation
dependencies: at most eight for trilinear and four for tetrahedral. Exact hits
on grid vertices, edges or faces skip zero-weight vertices; illegal data there
does not affect this observation. Each contributing vertex is a complete
three-component color. Any requested output channel computes/validates and returns
the complete color, with the corresponding atomic failure scope. Global axis
validation remains required independently of local table demand.

input rank is 2..8, last extent is 3 and logical element count is <=2^40.
A single color uses [1,3]. The observation domain is the remaining input axes,
rank 1..7, matching the current AtomKey rank constraint without introducing a
zero-dimensional observation domain.

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

Empty Q has no payload reads; static edges and descriptions are still preflight
validated. For nonempty Q, read/validate the entire axis[3,3] and all its derived
coordinates, then complete input colors at requested positions and exactly the
union of contributing full table vertices. Model legality is checked on the
original input before domain clamp, so clamp cannot hide nonfinite or invalid
source colors. Unrelated input colors and table vertices are not requested.
Typed/upstream support closures, if broader, are explicit and preserve provenance.

Axis changes invalidate all dependent observations. Input tuple changes affect
the corresponding output color; table vertex changes affect complete colors
whose exact nonzero support includes that vertex. Retain control/validation and
descriptor witnesses even when a constant table makes some numerical changes
invisible. Cache identity includes method, descriptors, grid, dtype and policy.
Cache-off and request partitioning cannot change values or failure scope.

Return immutable packed fragments with the correct global Region and storage
origin, complete-color channel coverage, and owning output description. Legal
unaligned, offset, negative/zero-stride input/table layouts are accepted. Retain
all source and descriptor backing owners needed by output/read windows beyond
ExecutionContext lifetime. A missing color is not silently zero-filled.

## Resource and failure contract

For M colors, grid validation is O(N0+N1+N2), lookup O(M*sum(log Ni)) and
arithmetic has at most 8 or 4 contributing vertices per color. Output payload is
3*M*sizeof(dtype). An optional cached Float64 grid needs 8*(N0+N1+N2) bytes.
Budget exact arithmetic limbs, vertex maps, source read windows/owners, output
fragments, descriptor storage and scratch growth overlap. No full table copy or
full logical output allocation is required. The 384 MiB maximal table payload is
not a promise that it fits a given execution budget.

Inherit CRV-06A's bounded cancellation polling and host capacity/work/stage
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
typed, stale, cancellation and resource categories and provenance. The observation
is one full output color, and no partial component success is published.

## Independent acceptance and public workflow

Use exact-rational vertex weights and component sums plus independent correctly
rounded dtype conversion. Identity tables, affine transforms, grid vertices,
faces/edges, split-plane ties, all six tetrahedra, all axis directions, unequal
axis extents, mixed dtypes and each supported model/description pair are required.
Use finite signed/HDR values and large opposite values to expose premature
rounding/overflow. Every CPU version matches the method's strict result bitwise;
trilinear and tetrahedral are not required to match each other.

For axis rows [0,1,1] and a 2x2x2 table at binary vertices (r,g,b), store
(r*g,g*b,b*r). Query [0.75,0.25,0.5] gives trilinear
[0.1875,0.125,0.375] and tetrahedral [0.25,0.25,0.5]. Use an RGB description
for this fixture. A polar-model table with hue 0 to 4 retains intermediate hue 2.
No modulo reduction can be substituted for that fixture.

Verify partial channel requests expand to full colors, zero-weight invalid
vertices remain unread, invalid demanded chroma fails even if domain-clamp would
hide it, axis errors are global, dirty support is exact, and all output metadata
matches the declared model/space. Include strides, owners after context teardown,
low budgets, cancellation, cache-off and joint versus partitioned requests.

The conceptual public workflow binds input/table/axis, explicitly supplies both
descriptions/dtype/policy, evaluates values through Compiler/ExecutionContext,
and inspects complete colors and output metadata. Implementations must provide
actual run commands and independently checked expected values. No current 3D LUT
implementation was found under plugins/ops; no runtime execution is claimed here.
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
