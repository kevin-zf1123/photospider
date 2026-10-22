---
spec_schema_version: 1
id: PTH-01D
parent_id: PTH-01
function: reverse_paths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.reverse_paths_v1_strict
---

# PTH-01D: reverse paths

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet; same-authority reversed PathSet, subpath order unchanged. Core output capacities are V+S, C+S, S; Primitive pool capacities are retained. Effective spline/arc records and their pool offsets may be compacted or remapped. Output counts describe the rebuilt prefixes; unused rows retain canonical zero padding.

## Parameters and domain

attribute_policy; Core,Bezier,ArcSweep/FullTurn,Hermite and nonperiodic spline support
as schema permits.

## Mathematical specialization

Open paths reverse segments/start point. Closed CoreVerbs paths retain the original M anchor, reverse the cycle and represent closure once with Z. Primitive paths use their own reference representation rather than M/Z. The v1 reverse domain requires a representable reverse within the same static pools; closed Primitive subpaths containing a terminal Close reference are outside that domain. In particular, an ArcSweep followed by a Close with non-Float64 endpoints cannot be reversed while retaining its start by inserting an ordinary Float64 Bezier line. This is an unchecked caller premise, not a new runtime admission scan or a promise to convert representations. Closed Primitive loops represented entirely by ordinary curve references, including FullTurn, retain their start as representability permits. Reverse Bezier controls; Hermite
becomes(P1,P0,-d1,-d0); negate arc direction; reverse spline controls/weights and map
knots k_i to a+b-k_(m-i). Mirror arc bindings within each original subpath length
interval, not whole-PathSet u->1-u.

Shared source slices are read immutably. For an effective control/weight pool of
N rows, reverse the complete paired pool once and map each spline's control
begin to N-control_begin-control_count. This preserves partial overlap,
containment and repeated references without overwriting another curve.

Each referenced spline receives its complete RN64 reflected knot sequence.
Compatible shared positions may retain their old offsets. Conflicting sequences
are placed in disjoint slices when capacity permits. Tight layouts eliminate
contained sequences and use maximum-weight suffix/prefix overlap matching on the
remaining nondecreasing sequences. Positive overlap edges form a DAG, so their
matched chains give minimum total storage for these prescribed sequences.
Containment offsets are resolved relative to their emitted owner.

ArcSweep uses RN64(theta0+sweep) and negated sweep; FullTurn retains theta0 and
negates its direction. References of both kinds may share an input arc row.
Preserve each reference kind and allocate or reuse its transformed eight-field
tuple independently. Identical transformed tuples can share one output row.
Unreferenced spline/arc records need not occupy the rebuilt effective prefixes.

The v1 geometry schema contains no attached attribute members. These geometry-only
ports do not consume separate width or source-map Results. Workflows carrying such
Results must apply the association rules explicitly; selecting an attribute policy
does not copy a separate Result implicitly.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

Complete-input staging and control/weight mirroring are linear in their stored
rows. Knot processing visits the referenced slices; the capacity-tight packing
fallback uses metadata linear in the static spline capacity and at most
O(T^3*L^2) exact-bit comparisons, where T is the number of retained referenced
spline records before containment elimination and L their maximum knot length.
The current implementation computes overlaps on demand. Arc tuple deduplication
uses an admitted hash table with full-tuple collision checks. Scratch is bounded
by static capacities; arithmetic, search and matching loops consume the work
budget and poll cancellation. New arc/knot values use RN64; output connections
remain subject to the representability premise without epsilon snapping.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Reverse twice restores geometry and orientation on the supported domain, allowing a redundant zero-length line, fixed closed Core anchor, 3-4-5 line, multisubpath width mapping and signed
derivative zeros. Also include overlapping control slices [0,1] and [1,2]
within the three-row pool [0,1,2], which must reverse to [1,0] and [2,1].
For tight knot packing, use K=[0,1,1,2,2,3] with two degree-one,
three-control records at knot_begin 0 and 1: their reversed five-knot slices fit
in the same six-row capacity after remapping to begins 1 and 0.
Cover contained/duplicate knot slices, nonmonotone first-reference order,
multi-level containment at nonzero output offsets, canonical tails after
compaction, and one arc row shared by ArcSweep and FullTurn with static capacity
for both transformed tuples.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
