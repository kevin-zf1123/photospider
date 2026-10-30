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

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.two_circle_radial_coordinate_v1_strict`, `generation.two_circle_radial_coordinate_v1_accelerated_apple_silicon`, `generation.two_circle_radial_coordinate_v1_accelerated_x86_64`.

c0[2],r0[1],c1[2],r1[1]; t[H,W] and UInt8 valid[H,W].

### Parameters and domain

canvas/dtype; nonnegative finite radii, distinct circles; no_solution=valid_zero default or reject.

`max_refinements` is an Int64 parameter, default 4096 (full positive Int64 cap). `no_solution` is a String, `valid_zero` (default) or `reject`. `valid_zero` publishes `(+0, 0)` for a query without a returnable coordinate; `reject` fails with `OperationFailed`, generation domain code `0x47454e0002`. Exhausting `max_refinements`, and any resource or cancellation failure, propagates as an error and never becomes `valid=0`.

## Mathematics

q=p-c0,d=c1-c0,s=r1-r0. Solve A*t^2+B*t+C=0 with A=dot(d,d)-s^2, B=-2*(dot(q,d)+r0*s), C=dot(q,q)-r0^2. Keep real roots with r0+t*s>=0; take the largest before spread. Linear cases use exact zero classification. If A=B=C=0, s<0 has maximum -r0/s; otherwise no finite maximum. No returnable coordinate gives (+0,0) or failure; success valid=1. Validity is decided on the exact root before output rounding; the radius r0+t*s=0 is admissible. The larger root is tested first and the smaller only if the larger is inadmissible. The selected root is rounded once to the output dtype (ties-to-even); an exact zero root gives +0 and a negative nonzero root that underflows gives -0.

## Behavior

Support class: Regional.

## Acceptance

Concentric 0->2 at distance 1 gives 0.5; equal-radius translated circles select larger root; no roots, tangency, linear and identity-equation cases; distinct circles are a caller premise. For `c0=(1.5,0.5), r0=1, c1=(3.5,0.5), r1=0` at `(0.5,0.5)`, select the admissible root `t=+0, valid=1`. Cover even/odd rounding midpoints, negative roots and signed subnormal underflow.
