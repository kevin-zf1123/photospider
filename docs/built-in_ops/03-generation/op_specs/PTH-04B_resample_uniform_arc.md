---
spec_schema_version: 1
id: PTH-04B
parent_id: PTH-04
function: resample_uniform_arc
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.resample_uniform_arc_v1_strict
---

# PTH-04B: resample uniform arc

Inherit [PTH-04](PTH-04_resampling_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet; PathSamplesArc Result with positions,source_segment,local_t,arc_s.

The associated Result uses `photospider.ops.path_samples_arc` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

count_per_subpath n>=1; hard max_count; open n1 takes start; closed endpoints not
repeated.

## Mathematical specialization

Open n>=2 targets s_j=jL/(n-1); closed uses jL/n. L0 repeats the valid start n times.
Invert true length; interior positive-length junction chooses following segment t0,
final open endpoint last valid segment t1. Position RN64(B(t*)) and local_t RN64(t*) are
independent; do not reconstruct position using rounded t.

## Runtime implementation

`path_arc_sampling.cpp` uses `path_arc.hpp` to integrate and invert true length with directed enclosures. `count_per_subpath` defaults to 2; `max_count` defaults to the subpath capacity times that count. `max_integration_panels=1048576` and `max_refinements=4096` are independent operational limits. A failed inverse/rounding certificate fails the call; it does not use a table-interpolated parameter. `generation::path_samples_schema` constructs the arc-sample schema.

Analytic line, monotone collinear polynomial and circular lengths retain the form `sqrt(q)`, optionally multiplied by `pi` for FullTurn. Exact rational-square tests group common square classes and prove normalized target/prefix equalities, including cancellation across several classes. A proved interior equality immediately selects the following positive-length segment with `local_t=0`, skipping zero-length runs. Unresolved general-curve comparisons retain the directed integration fallback.

For a monotone collinear Bezier/Hermite, exact coordinate comparisons invert its polynomial while the geometric target position is rounded independently from its exact endpoint interpolation. Higher-degree representations with zero higher coefficients use the constant-speed path. Continued-fraction reconstruction checks rational inverse candidates by exact substitution. Clearing the coordinate polynomial's denominators gives a bound `D` on every rational root denominator; a bracket of width less than `1/D^2` makes the midpoint convergents exhaustive by [Legendre's criterion](https://crypto.stanford.edu/pbc/notes/contfrac/converge.html). Exhausted reconstruction/refinement budgets remain certification failures. This does not provide symbolic identities for arbitrary transcendental curve lengths.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; certified inverse using table enclosures, not linear interpolation of an
approximate table.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

100px line five samples, closed400px square four corners, zero length and no
inter-subpath jump length. Include `(0,0)->(1,1)->(2,2)` with count 3: the middle sample is `(1,1)` on the second line at `t=0`; also scaled/mixed square classes, zero-length junction runs, a near-but-unequal junction, common-pi circular lengths, and monotone quadratic/cubic representations with rational and irrational inverse parameters.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
