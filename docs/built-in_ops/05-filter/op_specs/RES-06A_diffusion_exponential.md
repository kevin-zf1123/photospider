---
spec_schema_version: 1
id: RES-06A
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-06
function: diffusion_exponential
proposed_operation_keys:
- restoration.diffusion_exponential_strict
- restoration.diffusion_exponential_accelerated_apple_silicon
- restoration.diffusion_exponential_accelerated_x86_64
numeric_reference: S+B
oracle_scope: mathematical_reference
---

# RES-06A: Exponential conductance diffusion, fixed steps

Inherit [RES-06](RES-06_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Ports

`input` → `output` with identical shape. Each plane is an independent raw numerical field; channel count and color metadata do not imply RGB/vector/alpha behavior. Publish canonical planar output and applicable metadata under the shared contract.

## Required parameters

Require finite `kappa>0`, `dt>0`, `max_iterations>=0`, finite `dx>0`, finite `dy>0`, and explicit `y_axis` and `x_axis`. Require `dt <= 1/(2*(1/dx^2+1/dy^2))`. Boundary is the fixed no-flux definition. All caller controls must be supplied; constructors do not add defaults. Values use the actual stored Float64 parameter values. No normalization is inferred from dtype or image range.

## Recurrence and numeric stages

For each complete synchronous iteration and each undirected in-domain edge `(i,j)`, let `d=(u_j-u_i)/spacing_ij` and compute one baked64 conductance ``c_ij=RN64(exp(-(d/kappa)^2))``. Use that same coefficient in both edge directions and update `u_new[i]=RN_t(u_i+dt*Σ_j c_ij*(u_j-u_i)/spacing_ij^2)`. States use main input dtype `t`; do not calculate all iterations in a separate Float64 state. At zero iterations return input bit-copy. No external boundary flux is added. Do not claim bitwise mass conservation; stage rounding may cause small drift.

The profile is S+B: stage rounding is normative and conductance is a baked RN64 value. Ordinary arithmetic otherwise follows NUM. A finite legal-data parameter/control model is still explicitly validated; do not blanket-reject propagated NaN/Inf when NUM permits it. Do not clamp output.

## Demand and resources

The fixed-step member requests Whole per plane. The recurrence has a finite mathematical dependency, but this proposal does not authorize partial-state sharing. Control dependence includes the previous state’s conductance. The output descriptor depends only on parameters and input descriptor.

Work is `O(max_iterations*P*C)`, with two state generations and edge conductances. `P=H*W`; `C` is plane count. Account for buffers and intermediate state; budget failure is explicit, with no silent early stop, reduced precision, spilling, or partial publication. Cancellation is checked at safe iteration boundaries. Errors and output ownership follow FILTER common contracts.

## Reference and acceptance

The oracle function is `restoration.diffusion(image, *, kappa, dt=1/4, steps=1, dx=1, dy=1, exponential=True, dtype="float64")` in [restoration.py](../../../../oracle/ops/filter/oracles/restoration.py). Its `steps` argument corresponds to the proposed public `max_iterations` parameter. The `diffusion_exponential_one_step` fixture covers the formula with `DirectedMPFRStaged`; it establishes neither runtime support nor the complete acceptance contract. Check constant preservation, the two-pixel `[0,1]` case with `kappa=1, dt=1/4` yielding `RN_t([c/4,1-c/4])` with `c=RN64(exp(-1))`, conductance symmetry, zero steps, invalid stability bound, ROI/dirty demand, cancellation and resource failure. See [oracle README](../../../../oracle/ops/filter/README.md).

The proposed strict and accelerated keys are not registered. Accelerated variants must meet NUM’s final FP32-scaled four-ULP bound with required fallback. Float64 remains Float64; do not route through Float32. No runtime or performance evidence is implied.
