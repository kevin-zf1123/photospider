---
spec_schema_version: 1
id: PTH-02A
parent_id: PTH-02
function: evaluate_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.evaluate_bezier_path_v1_strict
  - path.evaluate_bezier_path_v1_accelerated_apple_silicon
  - path.evaluate_bezier_path_v1_accelerated_x86_64
---

# PTH-02A: evaluate bezier path

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

controls[S,d+1,2],segment_indices[N],t[N];
position[N,2],derivative[N,2],tangent[N,2],UInt8 tangent_valid[N].

## Parameters and domain

Int64 degree 1/2/3; Float32/64 controls/query, explicit output dtype; t in [0,1],
positive Tensor extents. Degree determines the static control shape. Check the
original Int64 value against 1..3 before any narrowing conversion, including
during empty-request preflight.

## Mathematical specialization

B(t)=sum binom(d,i)*(1-t)^(d-i)*t^i*P_i; derivative=d*sum B_i^(d-1)*(P_(i+1)-P_i). Round
complete component expressions once. Endpoints copy/convert controls. Interior exact
zero preserves -0 only when every control in that component is -0. Tangent validity uses
exact derivative, not rounded derivative.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries with the required control/index reads under unchecked payload premises; reference O(N*d^2).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5 line midpoint/derivative/unit tangent, cubic endpoint derivatives, exact-zero
tangent and reversal identity. Static degree checks reject 0, 4, 2^32+1,
2^32+2, 2^32+3 and negative values even if their low 32 bits match a supported
degree and the supplied control shape matches that truncated value.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
