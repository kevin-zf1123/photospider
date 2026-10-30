---
spec_schema_version: 1
id: CRV-11F5
parent_id: CRV-11
function: lowpass_nonuniform_gaussian
proposed_operation_keys:
  - curve.lowpass_nonuniform_gaussian_strict
  - curve.lowpass_nonuniform_gaussian_accelerated_apple_silicon
  - curve.lowpass_nonuniform_gaussian_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11F5: lowpass_nonuniform_gaussian

The dynamic inputs follow the [family Result tensor-port contract](CRV-11_nonuniform_lowpass_contract.md): each port is a Result with exactly one tensor member under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

The complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md) applies. Ordered dynamic ports are `positions` and `values` Results. `positions` has full `sample_shape()` [K], rank 1, finite strictly increasing Float32/64 values, K=2..1048576. `values` has full `sample_shape()` rank 1..8, positive shape and logical count <=2^40; static axis selects extent K. The output `samples` is a `photospider.tensor` v1 Result member preserving values shape/dtype with no attached domain description, evaluated at the original positions.

Required static parameters are axis, support_radius and sigma; boundary is reflect/replicate/zero/wrap, default reflect. All applicable numeric kernel parameters are finite; support_radius>0 uses position units. sigma>0 uses position units and support_radius truncates the Gaussian.

## Mathematics

The continuous gaussian formula is the corresponding shared kernel. It is integrated against the exact piecewise-linear extended signal and divided by the full continuous kernel integral. No uniform-grid approximation or edge renormalization defines this operator.

Strict correctly rounds the complete integral quotient. Accelerated permits <=4 ULP final error, exact constant/zero/sign and finite classification. Finite strictly increasing positions, finite contributing endpoint values and finite representable selected outputs are caller preconditions.

## Behavior

The output at a position depends on the piecewise-linear signal within support_radius of that position along the filtered axis, and on the complete positions vector; other-axis coordinates are independent. Region and tile results equal the corresponding slice of the whole result.

## Acceptance

The shared affine fixture positions=[0,0.75,2], values=[1,2.5,5] and support_radius=0.5 gives samples exactly 2.5 at the middle position for any admitted sigma parameters. Collinear-knot insertion invariance is verified against independent continuous integration. Also test constant/impulse hats, unequal spacing, boundaries/seams, extreme parameters, dtype mixing, 4-ULP bounds and local negative-zero support.
