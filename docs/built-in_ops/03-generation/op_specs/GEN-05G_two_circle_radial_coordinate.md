---
spec_schema_version: 1
id: GEN-05G
parent_id: GEN-05
function: two_circle_radial_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.two_circle_radial_coordinate_v1_strict
  - generation.two_circle_radial_coordinate_v1_accelerated_apple_silicon
  - generation.two_circle_radial_coordinate_v1_accelerated_x86_64
---

# GEN-05G: two circle radial coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

c0[2],r0[1],c1[2],r1[1]; t[H,W] and UInt8 valid[H,W].

## Parameters and domain

canvas/dtype; nonnegative finite radii, distinct circles; no_solution=valid_zero default
or reject.

## Mathematical specialization

q=p-c0,d=c1-c0,s=r1-r0. Solve A*t^2+B*t+C=0 with A=dot(d,d)-s^2, B=-2*(dot(q,d)+r0*s),
C=dot(q,q)-r0^2. Keep real roots with r0+t*s>=0; take the largest before spread. Linear
cases use exact zero classification. If A=B=C=0, s<0 has maximum -r0/s; otherwise no
finite maximum. No returnable coordinate gives (+0,0) or failure; success valid=1.

## Runtime implementation

`radial_two_circle.cpp` forms A, B and C from exact input dyadics. Linear and identically-zero cases are classified exactly. `A=B=0, C!=0` is no solution. For a quadratic, `bias=-B/(2A)` and positive `factor=sign(A)/(2A)` order the two roots as `bias +/- factor*sqrt(D)`. Test the larger root first, using an exact sign test for its radius; try the smaller root only if needed. Radius zero is admissible. Validity is decided on the exact root before output rounding.

The chosen root is rounded with certified affine-square-root arithmetic to the requested Float32/64. All three CPU profiles use this solver; architecture-named keys enforce their host architecture. `max_refinements` defaults to 4096. Work/cancellation/refinement failures propagate and never become `valid=0`. The `valid_zero` no-solution policy publishes canonical `(+0,0)`; `reject` uses generation domain code `0x47454e0002`.

Before interval rounding, `Math::affine_sqrt` detects whether the reduced nonnegative radicand has exact integer square roots in both numerator and denominator. A rational root is formed and rounded in the exact field, resolving exact cancellation to canonical `+0` and ties-to-even at Float32/64 rounding midpoints. A negative nonzero root that underflows still rounds to `-0`. The integer square-root scan is bounded by the arithmetic storage and charges work/cancellation; nonsquares retain interval certification.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(area(Q)); certified roots and exact branch/radius decisions; failures from budgets are
not invalid coordinates.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Concentric 0->2 at distance 1 gives 0.5; equal-radius translated circles select larger
root; no roots, tangency, linear and identity-equation cases; distinct circles are a caller premise. For `c0=(1.5,0.5), r0=1, c1=(3.5,0.5), r1=0` at `(0.5,0.5)`, select the admissible root `t=+0, valid=1`. Cover even/odd rounding midpoints, negative roots and signed subnormal underflow.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
