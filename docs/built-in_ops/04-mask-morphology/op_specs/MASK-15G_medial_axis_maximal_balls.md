---
spec_schema_version: 1
id: MASK-15G
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-15
function: medial_axis_maximal_balls
oracle_entry: medial_axis_maximal_balls
proposed_operation_keys:
  - mask.medial_axis_maximal_balls_strict
---

# MASK-15G: Maximal digital-ball centers and radii

Inherit [MASK-common](MASK_common_contract.md) and the
[discrete maximal-ball definition](MASK-15_topology_reconstruction_contract.md#discrete-maximal-ball-axis).
This is a mathematical specification, not a registered native implementation.

## Ports, parameters and output inference

input: Binary[H,W] -> axis: Binary[H,W], radius: Distance[H,W].
axis preserves the input Float32/Float64 dtype. radius uses required output_dtype
float32|float64. Both outputs have the input spatial basis. Required statics:
metric l1|l2|linf; positive finite Float64 sy,sx; distance_unit; output_dtype.
The unit is an explicit label for both spacings and radii, with no conversion.
There is no exterior, connectivity, pruning or representative-selection parameter.

## Mathematics

Every off-canvas lattice site is background. For every foreground center p,
r(p) is the exact nearest-background distance. Its digital ball is
B(p)={q in the integer lattice: distance(p,q)<r(p)}. Retain p iff B(p) is not a
proper subset of any other candidate ball. Equal support sets retain every
center. All predicates use exact rational distances or exact squared L2 distances.
Continuous-disk containment is not a substitute for digital-set inclusion.

At a retained center set axis=1 and radius=RN_T(r(p)); elsewhere emit +0 for
both outputs. This includes removed foreground centers and original background.
axis alone identifies centers: a positive exact radius can underflow to zero
in the output dtype. NaN/Inf input is invalid; finite radius overflow is an error
only for a requested radius observation. Axis-only requests do not round or
allocate radius values. Radius-only requests still compute center maximality.
All outputs use the same exact predicate; host joint/independent publication
rules remain applicable. This member never emits a no-feature infinity.

The exact retained balls reconstruct the original foreground by union. Rounded
radius samples do not certify that reconstruction. No one-pixel thickness,
connectivity, continuous-boundary width or minimal number of centers is promised.

## Demand and resources

Any nonempty axis or radius request requires Whole input Data/Validation.
Output buffers cover their requested global coordinates. Empty output demand
requires descriptor checks only. Any input change may dirty the full outputs.
There are no fixed-halo shortcuts to global maximality.

A small-image reference enumerates foreground candidates, exact nearest sites,
and digital ball sets, then tests proper inclusion. For N pixels it uses
O(N^2) support storage and O(N^3) naive set-inclusion work, in addition to exact
arithmetic bit costs. Production may optimize while preserving the exact sets.
Account for candidate distances, support/index state, retained inputs, output
and peak simultaneous scratch. Poll inside search and inclusion loops. Budget
exhaustion fails ResourceExhausted; cancellation and failure never publish an
incomplete axis. Oracle size caps are not native semantic limits.

## Independent acceptance

A single foreground pixel has axis=1 and radius=min(sy,sx) for all three metrics.
An all-background image has all-zero outputs. A full 3x3 unit-grid image under
Linf retains only its central pixel, with radius=2. Under L1 the center and four corners
remain: corner singleton balls are not contained in the central cross, while
edge-midpoint singleton balls are contained in it.
A full 2x2 unit-grid image retains all four centers with radius=1.

Test all three metrics, anisotropic spacing, equal support sets, exact strict
membership at the background radius, empty/full/edge-touching shapes, mixed
input/output dtypes, underflowed radius with axis=1, radius overflow, and
axis-only requests. Verify exact ball-union reconstruction using independently
constructed integer metric balls; do not reconstruct using rounded radii.

The [oracle](../../../../examples/mask_morphology_oracle/reference.py) entry is
medial_axis_maximal_balls. Native acceptance requires real public workflow,
Region, lifetime, budget and cancellation checks. [S25](../research-sources.md#s25)
supports the maximal digital-ball concept; this member fixes its own strict
radius and equal-support rules.
