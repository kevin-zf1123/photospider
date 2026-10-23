---
spec_schema_version: 1
id: PTH-03A
parent_id: PTH-03
function: bezier_arc_length
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.bezier_arc_length_v1_strict
  - path.bezier_arc_length_v1_accelerated_apple_silicon
  - path.bezier_arc_length_v1_accelerated_x86_64
---

# PTH-03A: bezier arc length

Inherit [PTH-03](PTH-03_arc_length_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; independent Float64 length[1] and ArcLengthTable Result.

The associated Result uses `photospider.ops.path_arclength_table` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

## Parameters and domain

epsilon_length_px>0 default1e-6; max_depth24,max_table_rows4096; no Float32 length.
Integration and refinement budgets are independent of the table tolerance.

## Mathematical specialization

Integrate speed exactly as target; total follows subpath order without jumps and
includes Z. Deterministic dyadic subdivision allocates epsilon/S across original
segments and halves leaf budgets on split, DFS left first. Chord lower/control-polygon
upper bounds enclose length. Store directed RN64 cumulative bounds. Both length outputs
equal RN64 true length, requiring additional refinement beyond table tolerance.

## Runtime implementation

`path_length.cpp` implements the CoreVerbs member; `curve_integral.hpp` provides certified speed integration. The true-length output and the table's `total_length` are independently rounded from the mathematical integral. The chord/control-polygon table is an enclosure and is not substituted for that integral. Defaults are `epsilon_length_px=1e-6`, `max_depth=24`, `max_table_rows=4096`, `max_integration_panels=1048576` and `max_refinements=4096`. CoreVerbs and Primitives are statically distinct authorities; this member requires CoreVerbs.

The strict and two architecture-named CPU profiles share certified arithmetic. Architecture admission is explicit; no GPU implementation is registered. Exhausted integration, refinement, exact-arithmetic or output capacity cannot publish a partial length/table.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input length/table, separately requested; O(leaves*d^2), charged stack/table.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5 length5, subdivided line100, collinear reversal, empty length0/table,
independently enclosed integrals; finite tolerance alone does not certify RN.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
