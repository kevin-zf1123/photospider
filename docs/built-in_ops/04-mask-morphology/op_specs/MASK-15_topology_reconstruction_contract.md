---
spec_schema_version: 1
id: MASK-15
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-15: Topology thinning, medial axis and geodesic reconstruction

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-15A](MASK-15A_thin_topological.md) | thin_topological | Row-major topology-preserving binary thinning |
| [MASK-15B](MASK-15B_thin_distance_ordered.md) | thin_distance_ordered | Distance-priority topology thinning with radius |
| [MASK-15C](MASK-15C_reconstruct_dilate.md) | reconstruct_dilate | Grayscale reconstruction by dilation |
| [MASK-15D](MASK-15D_reconstruct_erode.md) | reconstruct_erode | Grayscale reconstruction by erosion |
| [MASK-15E](MASK-15E_thin_zhang_suen.md) | thin_zhang_suen | Synchronous Zhang-Suen thinning |
| [MASK-15F](MASK-15F_thin_guo_hall.md) | thin_guo_hall | Synchronous Guo-Hall thinning |
| [MASK-15G](MASK-15G_medial_axis_maximal_balls.md) | medial_axis_maximal_balls | Maximal digital-ball axis and radii |

## Normative shared mathematics and interface

The initial scope includes project-defined thinning A/B, reconstruction C/D,
Zhang-Suen E and Guo-Hall F as independent members, and discrete
maximal-ball axis G. Its input is Binary on the pixel lattice. Candidate
centers are foreground pixel centers; their radii measure distance to background
pixel centers under an explicitly specified metric and spacing. No pixel-cell
boundary, half-pixel correction or contour reconstruction is implicit. Radius
therefore does not assert continuous shape thickness. G outputs a same-dtype Binary axis mask and a radius map in explicit
output_dtype, with +0 off the retained centers. E/F use their explicit
synchronous predicates; A/B retain their immediate-update definitions. The
member table counts specified contracts only.

### Discrete maximal-ball axis

Required metric is l1|l2|linf; sy,sx are required positive finite Float64
spacings in explicit distance units. For displacement (dy,dx), define
l1=abs(dy)*sy+abs(dx)*sx, l2_squared=(dy*sy)^2+(dx*sx)^2,
and linf=max(abs(dy)*sy,abs(dx)*sx). These are direct coordinate metrics,
not obstacle-constrained path distances. Stored spacings denote exact binary
values. L2 site ordering and ball membership compare exact squared distances;
L1/Linf compare exact rational distances. Radius output alone is rounded under
NUM; no rounded value may change a membership or maximality decision. Each
metric requires its own anisotropic and reconstruction acceptance cases.

The exterior is fixed background on every off-canvas integer lattice site;
there is no exterior parameter. All in-canvas foreground centers, including
canvas-edge sites, are eligible. Each candidate has a positive finite exact
nearest-background distance, even for a full foreground canvas. The nearest
exterior site can be sought on the one-pixel outside ring for these metrics.
Balls are defined on the complete lattice, without a separate canvas-clipping
step; their strict radius bound excludes all background sites and thus keeps
them inside the foreground set. An all-background input has no candidates and
an empty axis. A full foreground canvas is processed using exterior distances,
not an infinite-radius sentinel. If a finite exact radius cannot be represented
in the requested output dtype, the common arithmetic-overflow rule applies.

For each foreground center p let r(p) be its exact distance to the nearest
background center under the specified metric and exterior domain. Define the
digital ball B(p) as the lattice points q with distance(p,q)<r(p). Membership
uses exact distance predicates, not rounded radius output. The strict bound
excludes the nearest background site. An axis center p is retained iff no
foreground center t has B(p) as a proper subset of B(t). Inclusion compares
digital point sets, not continuous disk containment or radius magnitude alone.
All centers with equal maximal support sets are retained; no representative
selection, pruning or scan-order tie break is applied.

For a finite foreground domain with these well-defined inscribed balls, every
foreground center belongs to its own ball. The union of retained maximal balls
therefore equals the input foreground set. This is an exact digital-set
reconstruction statement; reconstruction from rounded radius samples alone is
not promised. The retained center mask need not be one pixel wide or connected
under a selected adjacency. It does not inherit thinning topology guarantees.

A/B are explicitly named project reference algorithms, NOT aliases for Zhang-Suen,
Guo-Hall, scikit-image skeletonize or a medial-axis transform. Input Binary;
output Binary skeleton in the input floating dtype. A uses a fixed row-major visit order of all initially
foreground sites. B orders those sites by (exact squared L2 distance to original
background INCLUDING the outside lattice, row-major index), ascending; required
sy,sx>0 finite. This original distance priority does not change during thinning.
B additionally outputs radius:Float32/64[H,W], from original-background distance
at surviving skeleton sites, +0 elsewhere; output_dtype required for radius.
It is a distance-ordered thinning radius, not a proof of maximal inscribed disks.

In each sweep visit remaining foreground sites in that fixed order. Changes
are IMMEDIATE, not simultaneous. Never delete a site with <=1 foreground
8-neighbor. For another site, tentatively delete it and accept only when BOTH
(1) total number of 8-connected foreground components and (2) total number of
4-connected background components in the image padded by one zero ring are
unchanged. Revert otherwise. Repeat complete sweeps until no deletion. This
finite, exact, deliberately conservative simple-point definition preserves
foreground components and holes under (8,4), including a 2x2 block; it can be
orientation/order biased and need not be unique, maximally thin or geometrically
centered. No pruning is done and existing one-neighbor endpoints are retained
when visited. Protecting semantic stroke endpoints beyond that local rule would
be a separate mask input/member. Termination follows strict decrease in number
of foreground sites on each successful sweep, at most N successful sweeps plus
one fixed-point sweep. A global-component-count oracle costs O(N³) in the worst
case, O(N) scratch. Production may replace global checks with PROVEN equivalent
local simple-point predicates, not a different deletion schedule. Poll inside
candidate connectivity checks, not just between sweeps.

C/D are grayscale geodesic reconstruction. Inputs marker,limit of identical
Coverage dtype/shape (or both Binary). Required connectivity 4|8; use the
center plus in-canvas neighbors only (neutral outside, not zero erosion).
C requires marker<=limit at every pixel. Start R0=marker and synchronously
iterate Rnext(p)=min(limit(p),max_{q adjacent or p}R(q)).
D requires marker>=limit; Rnext(p)=max(limit(p),min_{q adjacent or p}R(q)).
Stop at the exact fixed point; values are selected existing levels, not epsilon
convergence. Select center on extrema ties then row-major neighbor; for the outer
min/max tie select limit. Binary reconstruction uses floating 0/1 samples.
All input zeros in iterative state are canonicalized to +0 before iteration;
this explicit exception avoids signed-zero provenance oscillation. Fixed-point
comparison is then bitwise. No arbitrary user iteration count changes the output:
resource limits fail instead of returning an under-reconstructed success.

Synchronous reference O(iterations*N*degree), O(N) double buffer. Finite-valued
monotonic updates guarantee termination; an efficient queue algorithm must match
this exact final numeric result (and canonical zero), despite its different
schedule. Reconstruction by erosion is the numeric complement dual of dilation
under neutral boundaries. Whole marker/limit validation includes inequalities
at all pixels, even if a requested output could not be reached from that pixel.
All A-G have Whole dependence; radius output B depends on original binary input,
not only the thinned skeleton. Independent/joint output requests are numerically
consistent but require the same global skeleton if radius is requested.

## Sources, independent evidence and review boundary

[S18](../research-sources.md#s18); [S19](../research-sources.md#s19)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
