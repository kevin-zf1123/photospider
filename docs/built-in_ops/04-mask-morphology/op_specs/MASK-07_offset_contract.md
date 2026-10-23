---
spec_schema_version: 1
id: MASK-07
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-07: Discrete offsets, level sets and continuous polygon sampling

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
All members are registered in the bundled strict CPU set through fixed
invocation ABI 8; the specification status remains Proposed.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-07A](MASK-07A_offset_discrete.md) | offset_discrete | Physical-metric binary center offset |
| [MASK-07B](MASK-07B_shift_distance_field.md) | shift_distance_field | Shift a signed level set |
| [MASK-07C](MASK-07C_threshold_distance_field.md) | threshold_distance_field | Threshold a signed distance/level set |
| [MASK-07D](MASK-07D_offset_polygon_grid.md) | offset_polygon_grid | Continuous polygon offset with fixed sample-grid coverage |

## Normative shared mathematics and interface

Do not call these members interchangeable versions of one radius knob.
A is a binary center-set operator, B an arithmetic level-set shift, C a hard
level-set threshold, D a deterministic sampled rasterization of a continuous
polygon offset. Positive radius expands. A's results change only at discrete
lattice distances; D can give fractional coverage at noninteger radii.

A takes Binary; radius is finite signed Float64, sy,sx positive finite; metric
l1|l2|linf. Let B_r be integer offsets with metric((dy*sy,dx*sx),0)<=abs(radius).
For radius>=0 take max over B_r; for radius<0 take min, source zero outside D.
Radius ±0 copies bits under the Binary input premise. Membership is exact, using squared
radii for l2. Do not threshold a rounded distance to decide inclusion. Cost
O(Q|B_r|), finite support B_r. Halo bounds use floor(abs(r)/s) but the exact
metric stencil decides taps, not a rectangle. Capacity/work limits govern huge
radii; no slow-loop overflow is allowed.

B accepts a signed Distance (explicit basis and units), returning
RN_T(d-radius). This preserves a zero-level-set offset convention, not a proof
that the result is the true Euclidean SDF of a new shape after offset topology
changes. C returns [d<=radius] exactly in the field's dtype. A discrete signed
center-distance is never silently relabeled continuous SDF. No rounded shift
can be fused with C unless the specified branch is preserved.

D is included in the initial implementation scope as sampled polygon-offset
coverage, with exact sample membership and NUM rounding of the hit fraction.
Two explicit sample profiles are supported: grid_center and vulkan_standard.
Sampling does not establish an analytic pixel-area guarantee.

D takes one simple polygon `vertices: Float32/Float64[V,2]`, V>=3 in (y,x)
physical coordinates; implicit final-to-first closing edge. Finite vertices,
nonzero exact signed area, no repeated vertices, zero edges, self-crossings or
nonadjacent touching; adjacent edges may meet only at the shared endpoint.
Either orientation is accepted. Collinear intermediate vertices are allowed
when edges do not overlap. No holes, multiple rings, Bezier flattening or
soft-mask isocontour reconstruction is implied. Statics: positive height,width (H,W); sy,sx;
radius finite; output_dtype float32|float64; sample_pattern is required.
For grid_center, samples_per_axis is required Int64 1..64 and sample_count is
forbidden. For vulkan_standard, sample_count is required Int64 in {1,2,4,8,16}
and samples_per_axis is forbidden. Unknown profiles or irrelevant parameters
fail preflight. Authoring helpers may select grid_center with n=4 but must
serialize both fields; no runtime default is inferred.

For grid_center, N=n² and local coordinates are u=(j+.5)/n, v=(i+.5)/n.
For vulkan_standard, N=sample_count and (u,v) are the fixed pairs below divided
by 16, in sample-index order. u is x and v is y relative to the pixel's upper-left
corner. These tables are normative and are not queried from GPU hardware.

| N | (16u,16v), in order |
| --- | --- |
| 1 | (8,8) |
| 2 | (12,12), (4,4) |
| 4 | (6,2), (14,6), (2,10), (10,14) |
| 8 | (9,5), (7,11), (13,9), (5,3), (3,13), (1,7), (11,15), (15,1) |
| 16 | (9,9), (7,5), (5,10), (12,7), (3,6), (10,13), (13,11), (11,3), (6,14), (8,1), (4,2), (2,12), (0,8), (15,4), (14,15), (1,0) |

Both profiles evaluate the global physical point
`((y-.5+v)*sy, (x-.5+u)*sx)` in (y,x) order. The same point set is used in every
pixel, independent of ROI, tiling, traversal, threads or backend. Point positions
are exact rationals before geometric predicates. The Vulkan profile borrows
only sample positions, not Vulkan's triangle edge-ownership or rasterization
rules. Points on a polygon/offset boundary follow this member's inclusive rule,
including table points on a pixel edge. No random seed or implicit jitter exists.

A point is inside/on the polygon by exact winding/ray predicates; distance to
the nearest segment uses exact rational projection and squared distance. Let
d_P be negative inside, zero on the boundary, positive outside. Count points
with d_P<=radius, return RN_T(count/N). Radius<0 keeps interior points at least
abs(radius) from the boundary. Edge equality is included. This is exact for the
selected sample pattern but only an approximation to true pixel area: no uniform
area-error bound or commercial antialiasing match is asserted. Extremely thin
shapes can be missed. The profile, applicable count, fixed table definition and boundary rule are semantic
and enter the operation/static specialization identity. A contour-exact
coverage integrator would be a separate member.

D reads all vertices for any nonempty output selection and assumes the polygon
premises without a production polygon scan. Its reference work is O(Q*N*V),
with O(V) scratch; an oracle may additionally diagnose malformed polygons. Coordinate products and exact predicates are
budgeted. Continuous contour construction/reinitialization is outside this
bounded D2 integration draft, not an unspecified branch of A/B/C.

## Native implementation

The registered keys are listed in the member front matter. A/B/C/D use the
`values` output key. D serializes its output dimensions as positive Int64
`height` and `width`; H and W below denote those dimensions.
B/C require a nonempty String `distance_unit` and String `distance_basis`
chosen from `signed_center`, `continuous_sdf` and `level_set`.
These parameters declare the caller's interpretation; Results gain no attached
distance metadata.

[Discrete offset](../../../../plugins/ops/04-mask-morphology/offset.cpp)
builds a balanced site index over the exact support union of each selected tile.
Bounds may bind a rectangular input-tile superset, but samples outside the
metric stencil are not read as operands. Erosion includes the exterior zero
sites. Index scratch is O(S), where S is the clipped support rectangle size;
building and querying the index replaces the reference's repeated stencil scan,
without promising linear worst-case nearest-site work.
[Field operations](../../../../plugins/ops/04-mask-morphology/distance_fields.cpp)
use correctly rounded native subtraction or certified interval/exact-rational
evaluation. Contiguous ARM64 shift/threshold rows have strict NEON paths.
Radius zero preserves source bits.

[Polygon coverage](../../../../plugins/ops/04-mask-morphology/polygon_offset.cpp)
uses interval-certified winding and segment-distance predicates with exact
rational fallback. It implements both normative sample tables, inclusive
boundaries and either winding orientation. Scratch holds O(V) vertices and
bounded exact state; work remains O(Q*N*V). The hit fraction uses one native
division in the output dtype, with both integer operands exactly representable.
No polygon validity scan or contour reconstruction is added. All members
inherit the optional `max_work` guard and operational limits in MASK-common.

## Sources, independent evidence and review boundary

[S08](../research-sources.md#s08); [S09](../research-sources.md#s09); [S22](../research-sources.md#s22)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.

Distance inputs admit signed no-feature infinities and exclude NaN as a caller
premise. Finite shift preserves infinities; finite threshold and feather map
-Inf to 1 and +Inf to 0, as applicable. Finite results must be representable;
violated numerical premises have no runtime diagnostic guarantee.
