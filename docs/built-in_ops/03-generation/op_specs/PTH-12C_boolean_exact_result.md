---
spec_schema_version: 1
id: PTH-12C
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.boolean_exact_result_v1_strict
---

# PTH-12C: Exact polygon Boolean Result

Inherits [PTH-12](PTH-12_boolean_offset_contract.md),
[GEN-common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md). This
is a distinct registered versioned member. Same
version/profile means bit-identical public output; no backend substitution changes that
contract.

Inputs: two exact polygon Results, or explicitly lifted Float64 Polyline PathSets.
Lifting interprets every Float64 as its exact dyadic value; no coordinate rounding
occurs. Outputs: an exact polygon Result and a geometry report, associated to the same
operation snapshot. Parameters: union/intersection/difference/xor, independent
nonzero/evenodd fill rules, attribute_policy (default reject_unmappable), and separate
event/work/capacity budgets.

Compute regularized two-dimensional set operations using exact rational intersections
and predicates. Isolated points/zero-area edges are not area output. Canonicalize
contour orientation, collinear degree-two vertices, contour rotation and ordering as
specified by PTH-12A, using exact rational comparisons. Do not round coordinates. Store
exact rational coordinates and incidence/contour structure in the new formal Result; its
photospider.ops.exact_polygon v1 member directory and canonical rational representation are specified by the shared geometry contract. `generation::exact_polygon_schema` and the production codec are implemented in the public SDK and `path_boolean.cpp`, using the exact backend frozen by PTH-12. Subsequent exact
consumers operate on these exact values, not a hidden Float64 copy. Complete-input lowering declares fixed tasks that read all required geometry and
association members under their caller premises. Shared intermediate Results and
all dependencies are explicit in the Plan. Allocation/large-integer limbs, events and
retained backing must be charged; failure publishes no partial Result.

Acceptance: analytic rectangle operations; shared edges, holes and rational non-dyadic
intersections; exact-result chaining; permutation-independent canonical output;
acceptance checks of constructed association bytes and retained-owner lifetime on valid inputs; low-budget/cancellation failure. The independent slab oracle checks complete regularized regions; the separate rational halfedge reference checks canonical serialization.

The strict CPU implementation is registered. The acceptance list defines required behavior; finite validation does not establish every valid-input success domain or a GPU backend.
