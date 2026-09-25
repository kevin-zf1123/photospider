---
spec_schema_version: 1
id: PTH-12C
status: AcceptedDesign
implementation_status: not_implemented
registration_status: backend_and_schema_gates
---

# PTH-12C: Exact polygon Boolean Result

Inherits [D12](../decisions.md), [GEN-common](GEN_common_contract.md) and
[PTH geometry](PTH_geometry_contract.md). This is a distinct versioned member;
concrete registry spelling remains a freeze gate. Same version/profile means
bit-identical public output; no backend substitution changes that contract.

Inputs: two exact polygon Results, or explicitly lifted Float64 Polyline PathSets.
Lifting interprets every Float64 as its exact dyadic value; no coordinate rounding occurs.
Outputs: an exact polygon Result and a geometry report, associated to the same operation snapshot.
Parameters: union/intersection/difference/xor, independent nonzero/evenodd fill rules,
attribute_policy (default reject_unmappable), and separate event/work/capacity budgets.

Compute regularized two-dimensional set operations using exact rational intersections
and predicates. Isolated points/zero-area edges are not area output. Canonicalize
contour orientation, collinear degree-two vertices, contour rotation and ordering
as specified by PTH-12A, using exact rational comparisons. Do not round coordinates.
Store exact rational coordinates and incidence/contour structure in the new formal
Result; its codec, canonical rational representation and factory are schema gates.
Subsequent exact consumers operate on these exact values, not a hidden Float64 copy.
Whole execution reads complete geometry and associations. Allocation/large-integer
limbs, events and retained backing must be charged; failure publishes no partial Result.

Acceptance: analytic rectangle operations; shared edges, holes and rational non-dyadic
intersections; exact-result chaining; permutation-independent canonical output;
association/lifetime checks; low-budget/cancellation failure. No backend is selected.

These are specification acceptance requirements, not completed execution evidence.
