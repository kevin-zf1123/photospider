---
spec_schema_version: 1
id: PTH-geometry
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# Path geometry, associations and publication

Inherit [GEN common](GEN_common_contract.md) and the existing [structured
representations](../../../kernel-architecture/Structured-Representations.md). Existing
representation support does not implement these operators or new Result types.

## Geometry authority and attributes

Core verbs M/L/Q/C/Z have arity1/1/2/3/0. A single M is a legal zero-length open
subpath. Z terminates the subpath, agrees with closed and represents one actual closing
segment; do not add another closure edge. Empty paths have zero verb, control and
subpath rows and offsets=[0]. Primitive authority requires empty Core fields and
correctly associated references/payloads. Existing PathSet Float64 controls are finite,
in pixel-xy-right-down coordinates. Current schema limits include 1,048,576 segments,
4,194,304 controls and spline degree<=16; verify the actual public schema before
integration rather than defining a private Python type.

A copied Result gets rebuilt associations, never stale source ObjectIds. Concat
preserves port and subpath order. Dynamic splitting returns a flat PathSet and partition
table with fixed output ports, not nested Results or variable arity.

Existing attribute domains and interpolation enums do not automatically express new
width binding semantics. Default reject_unmappable; explicit drop_unmappable reports
removed keys/reasons and preserves the remaining mappable fields. Width is finite
nonnegative full diameter in pixels, with exactly one explicit attached or independent
source. Bindings distinguish whole-path normalized length, subpath normalized length and
subpath physical arc length. Whole-path accumulation excludes jumps between subpaths.
Formal schema details, zero-length and seam cases must be defined before registration;
do not reinterpret a legacy ArcLength tag. PCHIP evaluation does not invent a PCHIP enum
in an existing attribute codec.

Trim/dash retains original position/parameter and source-length mappings; explicit
rebinding alone spreads the width function across each output fragment. Rounded output
controls can alter length, so do not equate new arc length with the source interval
length. The member specifies exact source queries for all three domains.

## Evaluation and canonical boundaries

Absolute-control Bezier evaluation uses the full Bernstein/de Casteljau expression and
one final RN. CRV relative-handle reconstruction has its own explicit RN64 stage;
adapters must preserve it. Local t is in[0,1]; endpoint position copies control bits
after required dtype conversion. Derivative is dP/dt. Zero derivative has zero tangent
and valid0; tangent requests must not divide by zero.

Arc position is c+u*cos(theta0+t*sweep)+v*sin(theta0+t*sweep). FullTurn endpoints reuse
the start result under the specified direction, not rounded sin(2*pi). B-spline t maps
to [k[d],k[n]], with the final endpoint using the left limit and interior knots the
member's specified right limit. Core segment IDs are verb rows (M is not evaluable, Z
closes current point to M); primitive IDs are reference rows.

At cumulative arc-length junctions choose the following positive-length segment, except
the final endpoint; skip zero-length segments. Zero total length with a valid point
remains a valid point, while an actually empty path follows each member's explicit
empty/NoSolution behavior.

## Length and sample Results

True length s(t)=integral_0^t norm(P'(u)) du. Straight segments are analytic. Bezier
dyadic subdivision can bound length with chord lower/control-polygon upper bounds and
directed accumulation. Ordinary quadrature error estimates or matching high-precision
values are not correct-rounding certificates. RN64 true length requires refinement to a
unique rounding cell or an exact tie proof even after a table tolerance is met;
exhausted certification fails.

ArcLengthTable is a formal Result design: source segment IDs and ranges, ordered t knots
with cumulative lower/upper s bounds, totals and a correctly-rounded length. Fields bind
to the source path snapshot and table identity. PathSamples variants carry positions,
source segment and local t, with arc_s only when computed. PathPartition and quality
reports also need factories/codecs/association validation. These logical fields are not
evidence that current representation.hpp implements them.

## Deterministic flattening

For Bezier degree d, split exactly at t=1/2, DFS left first. Let Q_i be the degree-d
control vector of the endpoint chord: Q_i=(1-i/d)*P0+(i/d)*Pd. Bounding every
norm(P_i-Q_i) bounds parameter-matched distance; perpendicular-only flatness would miss
collinear reversals. Include maximum published endpoint RN64 displacement in the leaf
bound. Default maximum depth24 and output-segment limit1,048,576 are explicit budgets.
If Float64 has insufficient publication precision, more subdivision may not help: report
InvalidQuality instead of silently changing coordinates.

Use epsilon_geom_px>0, default0.05 target-canvas pixels, explicit preview0.25. These are
engineering presets, not measured optimality claims. Persist the actual parameter.
Subsequent transforms propagate/revalidate error. Strict arithmetic on the published
polyline does not imply strict original-curve area or distance. Distance bounds do not
establish coverage or topology bounds. Default allow_change reports unverified topology
unless actually checked; reject_unproven requires the member's defined proof. Never
relax epsilon to hide budget exhaustion.

## Filled regions, Boolean and distance

Open contours close at fill consumption without mutating the source. Default
nonzero/evenodd selection is explicit. Single points and zero-area edges contribute zero
coverage. Half-open ray crossing defines boundary ownership, without changing area.
Regularized Boolean output contains area, not isolated points or lines. Predicates and
discrete topology cannot use accelerated ULP tolerance.

Exact rational Result, Float64 publication and explicit-grid algorithms are distinct
members. Float64 publication checks displacement, incidence and newly created
crossings/merges. Grid members must specify quantization and backend degeneracy
behavior. Never silently replace exact-input geometry with a grid result. Exact line
intersections do not imply support for circular/offset constructions.

Distance uses the exposed boundary of the selected region, excluding internal
shared/overlap edges. Min of individual SDFs is not general exact distance inside a
union. Closest ties use (source subpath,segment,t) lexicographically, smaller t first;
generated Boolean boundaries use canonical edge order. Boundary SDF is +0. An empty
region permits +max_distance only in an explicitly truncated distance member; a
requested missing closest point fails NoSolution.

## Stroke geometry

Width0 gives empty coverage, never implicit hairline. Caps are round/butt/square; joins
round/bevel/miter. Miter limit>=1 compares outer corner distance divided by half-width;
equality retains miter, excess uses bevel. Resolve self-overlap as a geometric union
before coverage, not repeated source-over. Closed paths have no end caps. A lone point
or zero-length subpath gives a radius-width/2 disk for round, a centered axis-aligned
width square for square, or empty for butt. Reversal joins must be finite and follow the
same union construction.

Variable-round strokes use the explicitly defined variable-disk sweep, not a
constant-radius offset formula. Curved boundary approximation is separately named and
reports its geometry error. Centerline transforms preserve width in pixels; outline
transforms move the whole already-stroked object.

## Fitting, support and resources

Each fitting backend solver is its own versioned operator with fixed initialization,
split/tie/stop behavior. Shared continuous Hausdorff acceptance includes publication
error and endpoint/locked/corner constraints. Discrete residuals are insufficient; no
minimum-segment guarantee or silent solver fallback is provided. Default topology policy
is allow_change, with optional reject_unproven and explicit proof requirements.

Raster output is Regional; complete path validation/control reads may remain
Conservative. A BVH may discard only proven irrelevant candidates, including tie cases.
Dirty stroke bounds include width, caps, miter extension and geometry error, not only
control-point bounds. Length/binding changes can have global effects. Charge exact
arithmetic, intersections (worst-case quadratic), indexes, dynamic backing and
validation work; output pages do not imply an inherently streaming algorithm. Public
execution must validate cancellation and retained-owner lifetime.
