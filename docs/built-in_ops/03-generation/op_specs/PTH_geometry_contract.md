---
spec_schema_version: 1
id: PTH-geometry
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: partially_implemented
---

# Path geometry, associations and publication

Inherit [GEN common](GEN_common_contract.md).

## Current implementation scope

GEN-01..04, NOI-01..04, PTH-01..02 and PTH-09A are implemented.
Other schemas and members in this shared contract retain their own implementation
status.

## PathSet schema family

PathSet is a logical type family with separate CoreVerbs and Primitives schemas. A CoreVerbs Result contains only the M/L/Q/C/Z representation; a Primitives Result contains only typed primitive records and their payloads. Consumers explicitly support the relevant schema. Converting representations requires an explicit operation, with its declared mathematical and approximation rules. Width and source mappings live in the separate associated Results defined below.

The CoreVerbs schema is `photospider.ops.path_core`, version 1, with positive static capacities V, C and S:

| Member | Type and shape | Meaning |
| --- | --- | --- |
| `verbs` | UInt8 `[V]` | Valid verb prefix. |
| `control_offsets` | Int64 `[V+1]` | Control boundaries for the valid verb prefix. |
| `controls` | Float64 `[C,2]` | Finite absolute x/y controls. |
| `subpath_offsets` | Int64 `[S+1]` | Verb boundaries for valid subpaths. |
| `closed` | UInt8 `[S]` | Closed flags for valid subpaths. |
| `counts` | Int64 `[3]` | Effective verb, control and subpath counts, in that order. |

Counts are bounded by V/C/S respectively. An empty path has all three counts zero and the first offset in each offset member equal to zero. Verb codes M/L/Q/C/Z are 1/2/3/4/5, and valid closed flags are 0/1. Unused verbs and closed flags are zero; unused controls are positive zero. control_offsets entries after the effective verb terminator repeat the effective control count; subpath_offsets entries after the effective subpath terminator repeat the effective verb count. Thus a valid full offsets array remains nondecreasing, and the empty representation is all zero. These are unchecked payload invariants, not validation obligations. The Core constructor takes the six field tensors explicitly; counts is an ordinary input, not inferred by inspecting verbs or offsets. Capacities V/C/S come from the input shapes, with offsets of size V+1/S+1. Concat sums corresponding input capacities with checked static arithmetic; Core reverse reserves V+S verb rows and C+S control rows while retaining S subpaths, so a reversed old closing edge can be explicit when the first segment is curved. Affine centerline transform and stable split reuse input capacities. Reverse-twice equivalence is geometric; a redundant zero-length line can remain. Algorithms whose output bound is not structurally derivable take explicit max_verbs/max_controls/max_subpaths capacities. A logical zero bound uses one physical row and zero effective count. Insufficient output capacity is an operational failure, never permission for dynamic extent growth.

All six members are non-spatial with no batch axes. controls has component axis 1 and one trailing atomic axis; counts has one atomic trailing axis containing its three entries. Other members use scalar observations and no component axis. One CompleteBundle scope covers all members. Lowering explicitly writes the complete fixed capacity, including padding, and the workflow exports the complete value; the metadata does not add automatic validation or scheduling. Existing physical strides/offsets apply. The public generation SDK provides Core and Primitive schema factories.

## Shared representation rules

All schemas below have version 1 and at most 16 fixed Tensor members. A capacity is a static parameter or is derived from input shapes. A logical zero capacity uses physical capacity one and a zero effective count; offset arrays have physical capacity plus one. Capacity arithmetic is checked as static structure. Exceeding an algorithm's output or limb budget is an operational failure, not a request to grow the schema.

Only valid prefixes participate in mathematics. Unused ordinary rows are zero, with Float64 positive zero; unused offset entries repeat the valid terminal value. Closed flags, counts, offsets, indices, references, finite values, rational normalization and source pairing are unchecked premises. A producer writes canonical output by construction, without an additional validation pass.

Members are non-spatial with no batch axes. Scalar arrays use scalar observations. A row containing a complete point, fixed record, tuple of counts or ObjectId bytes is atomic across its trailing field axes; logical component axes identify point/vector coordinates only. Physical strides and offsets follow the existing Tensor interface. CompleteBundle covers the members of each Result; lowering explicitly supplies complete coverage and successful retirement. No schema policy supplies missing reads, writes or semantic checks.

Source ObjectIds are eight little-endian UInt8 bytes copied from known input identities, with no comparison or hashing. Producers containing such process-local identity bytes declare deterministic=false; numerical and discrete fields retain their specified deterministic behavior. Source ResultRefs needed for later calculations are explicit workflow/SDK arguments, not hidden references reconstructed from an ID. Except for the explicitly bound GEN-08 point-generation report, a geometry Result and its accompanying metadata are paired by the caller; metadata does not acquire a new target-output ObjectId or introduce a binding stage merely for validation.

## Primitives schema

`photospider.ops.path_primitives` has the following 13 members. Counts are effective prefixes; R/S/B/A/H/T/U/K denote their positive static capacities.

| Member | Type and shape | Meaning |
| --- | --- | --- |
| `references` | Int64 `[R,2]` | kind and payload row. |
| `subpath_offsets` | Int64 `[S+1]` | Reference ranges for each subpath. |
| `closed` | UInt8 `[S]` | 0/1 flags. |
| `isolated_positions` | Float64 `[S,2]` | Position of a subpath with no references; zero for other subpaths. |
| `bezier_degrees` | Int64 `[B]` | 1, 2 or 3. |
| `bezier_controls` | Float64 `[B,4,2]` | Absolute controls; unused control slots are +0. |
| `arcs` | Float64 `[A,8]` | cx, cy, ux, uy, vx, vy, theta0, sweep_or_direction. |
| `hermite_controls` | Float64 `[H,4,2]` | P0, P1, d0, d1. |
| `splines` | Int64 `[T,5]` | degree, control_begin, control_count, knot_begin, knot_count. |
| `spline_controls` | Float64 `[U,2]` | Control pool. |
| `spline_knots` | Float64 `[K]` | Knot pool. |
| `spline_weights` | Float64 `[U]` | Positive weights aligned with controls; effective nonrational weights are 1. |
| `counts` | Int64 `[8]` | Effective R,S,B,A,H,T,U,K. |

Reference kinds are 1=Bezier, 2=ArcSweep, 3=FullTurn, 4=Hermite, 5=B-spline, 6=Close; zero is padding. FullTurn stores direction exactly -1 or +1 in the final arc field and uses the mathematical direction*2*pi sweep. Close uses its payload row as the owning subpath index and represents the exact line from the preceding segment's mathematical endpoint to that subpath's mathematical start. It is terminal and has no floating control payload. This represents closure of an arc whose endpoint is not exactly representable as Float64 without inventing a rounded Bezier endpoint. A FullTurn or other already closed loop need not add a second closing edge. A subpath with no references uses its isolated position; exact connectivity and closure are caller premises.

Constructor capacities come from the corresponding input shapes. Concat sums each pool capacity; reverse and a representable affine transform retain them. Other producers take explicit capacities for the pools they can generate. Primitive reference rows are segment IDs, including an explicit Close row. Shared references may reuse a payload row; the caller supplies valid indices. The schema adds no new curve evaluation algorithm.

## ArcLengthTable schema

`photospider.ops.path_arclength_table` has seven members:

| Member | Type and shape | Meaning |
| --- | --- | --- |
| `segment_ids` | Int64 `[E]` | Original Core verb or Primitive reference rows. |
| `segment_offsets` | Int64 `[E+1]` | Knot ranges for each segment. |
| `knots` | Float64 `[N,3]` | local_t, cumulative_s_lower, cumulative_s_upper. |
| `subpath_offsets` | Int64 `[S+1]` | Segment ranges per source subpath. |
| `totals` | Float64 `[S+1,3]` | RN64 length, lower bound, upper bound; row 0 is the whole path, subsequent rows are source subpaths. |
| `counts` | Int64 `[3]` | Effective segments, knots, subpaths. |
| `source_object_id` | UInt8 `[8]` | Source path identity. |

E/S are bounded by source segment/subpath capacity; N is max_table_rows. Knot s bounds accumulate in path traversal order without subpath jumps. Each stored local_t denotes its exact Float64 value, and the bounds refer to that value, not to an unrecorded rounded parameter. Internal integration may refine beyond the published knot table. Empty paths have zero counts and totals. The independent length output equals totals[0,0]. Table tolerances and stored intervals do not substitute for the member's correctly-rounded true-length computation.

## PathSamples and PathPartition schemas

`photospider.ops.path_samples_param` has positions Float64 `[P,2]`, source_segment Int64 `[P]`, local_t Float64 `[P]`, subpath_offsets Int64 `[S+1]`, counts Int64 `[2]` (points, subpaths), and source_object_id UInt8 `[8]`. Output rows are grouped in source-subpath order. An isolated point uses source_segment=-1 and local_t=+0; its source subpath is given by the offset group. The local_t value is independently rounded and does not reconstruct the position or define an exact source map.

`photospider.ops.path_samples_arc` adds arc_s Float64 `[P]`, the independently rounded physical arc distance within the corresponding source subpath. It is present only when computed by an arc-sampling member. P is a static bound from the requested fixed sample count and source capacity, or max(1,max_count); S comes from source capacity. Effective prefix and padding rules apply.

`photospider.ops.path_partition` has group_offsets Int64 `[G+1]`, subpath_count Int64 `[1]`, and source_object_id UInt8 `[8]`. G is the static group_count. Offsets partition the paired flat output PathSet's subpaths; empty groups repeat an offset. Geometry and row order are not duplicated in this metadata.

## Linear width bindings

`photospider.ops.path_width` represents only the standard width field with attached Linear profiles. It has profiles Int64 `[P,4]` (source_slot, domain, stop_begin, stop_end), bindings Int64 `[B,3]` (target_subpath, profile_row, source_subpath), stops Float64 `[K]`, widths Float64 `[K]`, source_object_ids UInt8 `[O,8]`, and counts Int64 `[4]` (bindings, profiles, stops, sources). P/B/K/O are static capacities from inputs or explicit output limits; concat sums the necessary capacities.

Domains are 0=pathset_normalized_arclength, 1=subpath_normalized_arclength, 2=subpath_arclength_px. A path-wide profile uses source_subpath=-1; subpath-specific profiles use a valid source row. The standard key is the literal width and is not stored as a string table. Profile stop domains, nonnegative widths, bindings and source identity are unchecked. No other dynamic attribute keys or generic key/value codec are introduced. PCHIP remains the existing independent evaluation workflow and does not acquire an attached interpolation code.

Attach-linear-width returns the unchanged complete path value paired with its width Result. Geometry is not copied to attach metadata. Existing metadata replacement follows the explicit replace_existing configuration; a conflicting existing binding when replacement is disabled is outside the caller premises. After geometry operations, the returned width Result is paired with the new geometry by the workflow; no consumer validates that pairing. Drop-unmappable reports the width key and unmappable reason using the geometry report below.

## Exact rational fields and source maps

A rational field with logical shape Q uses signs UInt8 `[Q...]`, sizes Int64 `[Q...,2]` and limbs UInt8 `[Q...,2,L,4]`. The first added limb-array axis selects numerator/denominator; L is a static limb capacity, each base-2^32 limb is four little-endian bytes, and limbs are least-significant first. Signs are 0 for nonnegative and 1 for negative. Effective rationals are reduced, have a positive denominator and positive used limb counts no larger than L. Zero uses sign=0, numerator size=1 with limb 0, denominator size=1 with limb 1. Higher unused limbs are zero. Entire ineffective rows have zero sizes and bytes and are excluded by the containing count. These are representation premises, not runtime normalization checks; the producing exact arithmetic constructs its canonical result.

`photospider.ops.path_source_map` has records Int64 `[M,4]` (target_segment, source_slot, source_subpath, source_segment), source_object_ids UInt8 `[O,8]`, affine_signs UInt8 `[M,2]`, affine_sizes Int64 `[M,2,2]`, affine_limbs UInt8 `[M,2,2,L,4]`, and counts Int64 `[2]` (mappings, sources). Each row maps an entire target segment by t_source=a+b*t_target, with a/b stored as exact rationals. M is the target segment bound; O/L are static source/limb capacities. Source geometry and length tables remain explicit inputs when width evaluation requires them.

This v1 map supports identity, reverse, exact rational subintervals and source reindexing. A nonrational true-arc cut, or a target segment spanning multiple source segments without one such affine map, is not represented by a rounded Float64 surrogate. Existing reject_unmappable or explicit drop_unmappable applies when an operation cannot construct the required mapping. No expression graph, new solver protocol or implicit width rebind is added. This is an output-representation limitation for valid inputs, not a payload validator.

## Exact and grid polygon schemas

`photospider.ops.exact_polygon` has coordinate_signs UInt8 `[V,2]`, coordinate_sizes Int64 `[V,2,2]`, coordinate_limbs UInt8 `[V,2,2,L,4]`, contour_offsets Int64 `[C+1]`, and counts Int64 `[2]` (vertices, contours). It uses the rational encoding above. Contours store cyclic vertices without repeating the initial vertex; consecutive vertices and cyclic closure define edges. Coincident exact coordinates preserve contact relations. No separate general graph representation is required. Orientation, contour rotation and ordering follow PTH-12A. V/C/L are explicit static capacities; exhausting them fails operationally without substituting Float64 or grid geometry.

`photospider.ops.grid_polygon` has coordinates Int64 `[V,2]`, contour_offsets Int64 `[C+1]`, and counts Int64 `[2]`. Grid origin and positive spacing remain explicit static operation parameters, supplied consistently to producers and consumers. Grid coordinates are signed64; required input and output range representability is a caller premise. The schema does not select a Boolean backend or its intermediate intersection-rounding policy. Those mathematical choices remain separate implementation gates; they cannot be inferred from this storage layout.

## Minimal quality reports

`photospider.ops.path_fit_report` has error_bound_px Float64 `[1]`, topology_status Int64 `[1]`, and source_object_id UInt8 `[8]`. The error is the algorithm's certified bidirectional Hausdorff upper bound including coordinate publication. Topology codes are 0=unverified, 1=preserved, 2=established change. Solver identity is the explicit operation/version; no trace, residual history or solver-name payload is added. Nonconvergence and certification failure use the existing operation failures.

`photospider.ops.geometry_report` has error_bounds_px Float64 `[3]` (approximation, quantization, publication), topology_status Int64 `[1]`, dropped_width UInt8 `[1]`, and source_object_ids UInt8 `[O,8]`, where O is the static source arity. Unused error stages are +0. dropped_width=1 means the standard width key was explicitly dropped as unmappable; 0 means retained or absent. This records the existing policy without a generic reason or attribute dictionary. Error/topology fields are obtained from the operation's required mathematics, not from extra publisher/consumer scans. Source maps are provenance support for those bindings, not another user attribute.

## Geometry authority and attributes

Core verbs M/L/Q/C/Z have arity1/1/2/3/0. A single M is a legal zero-length open
subpath. Z terminates the subpath, agrees with closed and represents one actual closing
segment; do not add another closure edge. Empty paths have zero verb, control and
subpath rows and offsets=[0]. The separate Primitives schema requires correctly associated references/payloads; it has no CoreVerbs fields.

A copied Result gets rebuilt associations, never stale source ObjectIds. Concat
preserves port and subpath order. Dynamic splitting returns a flat PathSet and partition
table with fixed output ports, not nested Results or variable arity.

The path_width schema below defines the three source domains and attached Linear binding; independent PCHIP evaluation uses its own member contract. Default reject_unmappable; explicit drop_unmappable reports removed keys/reasons and preserves the remaining mappable fields. Width is finite nonnegative full diameter in pixels, with exactly one explicit attached or independent source. Bindings distinguish whole-path normalized length, subpath normalized length and subpath physical arc length. Complete-input-path accumulation excludes jumps between subpaths. Zero-length and seam behavior follows the width members. PCHIP evaluation alone does not establish an attribute codec or interpolation encoding.

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

The ArcLengthTable schema records source segment IDs and ranges, ordered t knots
with cumulative lower/upper s bounds, totals and a correctly-rounded length. Fields bind
to the source path snapshot and table identity. PathSamples variants carry positions,
source segment and local t, with arc_s only when computed. PathPartition and quality
reports use the schemas above. Their runtime factories/codecs remain unimplemented.

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
Subsequent transforms propagate the declared error or compute a new algorithmic certificate; publishers and consumers do not rescan it. Strict arithmetic on the published
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
members. Exact-to-Float64 conversion constructs rounded coordinates and accumulates
their displacement bound. Representability, the requested displacement budget and
preserved incidence/topology are unchecked caller premises, with no post-rounding
geometry scan or publication gate. Conversion reports unverified topology (code 0);
an explicit topology-analysis operation may compute a different status. Grid members
use the same publication premises and must specify quantization and backend degeneracy
behavior. Never silently replace exact-input geometry with a grid result. Exact line
intersections do not imply support for circular/offset constructions.

For polygon publication, let each exact vertex be (x_i,y_i) and its constructed
RN64 vertex be (X_i,Y_i), expressed in the same geometric pixel coordinates.
While constructing vertices, accumulate exactly
`M=max_i((X_i-x_i)^2+(Y_i-y_i)^2)`, starting from zero. The reported publication
bound is `RU64(sqrt(M))`: the least Float64 value not below the exact square root,
with empty output and exact zero reported as +0. This also bounds corresponding
line-segment displacement under the same linear edge parameterization. The
reporting calculation neither compares this bound with epsilon to authorize
publication nor checks rounded topology. Required report representability is an
unchecked numerical premise; arithmetic work/capacity exhaustion still fails.

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

Raster output is Regional; complete mathematical control reads may remain conservative. A BVH may discard only proven irrelevant candidates, including tie cases.
Dirty stroke bounds include width, caps, miter extension and geometry error, not only
control-point bounds. Length/binding changes can have global effects. Charge exact
arithmetic, intersections (worst-case quadratic), indexes, dynamic backing and
algorithmic certification work; output pages do not imply an inherently streaming algorithm. Public
execution must honor cancellation and preserve retained-owner lifetime.
