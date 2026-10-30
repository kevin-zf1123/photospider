---
spec_schema_version: 1
id: RES-06B
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-06
function: diffusion_reciprocal
proposed_operation_keys:
- restoration.diffusion_reciprocal
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-06B: Reciprocal conductance diffusion, fixed steps

Inherit [RES-06](RES-06_contract.md), the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Ports

`input` and `output` are Results with matching fixed Tensor schemas. Each plane is an independent raw numerical field; channel count does not imply RGB/vector/alpha behavior. Each materialized output tile has a distinct allocation. Downstream color interpretation, if needed, is explicit caller-supplied configuration.

## Required parameters

Require finite `kappa>0`, `dt>0`, `max_iterations>=0`, finite `dx>0`, finite `dy>0`, and explicit `y_axis` and `x_axis`. Require `dt <= 1/(2*(1/dx^2+1/dy^2))`. Boundary is the fixed no-flux definition. All caller controls must be supplied; constructors do not add defaults. Values use the actual stored Float64 parameter values. No normalization is inferred from dtype or image range.

## Recurrence and numeric stages

For each complete synchronous iteration and each undirected in-domain edge `(i,j)`, let `d=(u_j-u_i)/spacing_ij` and compute one native input-dtype conductance ``c_ij=1/(1+(d/kappa)^2)``. Use that same coefficient in both edge directions and update `u_new[i]=RN_t(u_i+dt*Σ_j c_ij*(u_j-u_i)/spacing_ij^2)`. States use main input dtype `t`; do not calculate all iterations in a separate Float64 state. At zero iterations return input bit-copy. No external boundary flux is added. Do not claim bitwise mass conservation; stage rounding may cause small drift.

The profile is F: native state and conductance use the main input dtype, while synchronous iteration boundaries remain normative. The finite parameter/control-model and stability domains are caller premises; do not blanket-reject propagated NaN/Inf under the native arithmetic contract. Do not clamp output.

## Demand and resources

The fixed-step member requests complete-input per plane. The recurrence has a finite mathematical dependency, but this proposal does not authorize partial-state sharing. Control dependence includes the previous state’s conductance. The output descriptor depends only on parameters and input descriptor.

Work is `O(max_iterations*P*C)`, with two state generations and edge conductances. `P=H*W`; `C` is plane count. Account for buffers and intermediate state; budget failure is explicit, with no silent early stop, reduced precision, spilling, or partial publication. Cancellation is checked at safe iteration boundaries. Errors and output ownership follow FILTER common contracts.

## Reference and acceptance

The oracle function is `res06.restoration.diffusion(image, *, kappa, dt=1/4, steps=1, dx=1, dy=1, exponential=False, dtype="float64")` in [res06/restoration.py](../../../../oracle/ops/filter/res06/restoration.py). Its `steps` argument corresponds to the proposed public `max_iterations` parameter. The `diffusion_reciprocal_one_step` fixture covers the formula with `ExactRationalStaged`; it establishes neither runtime support nor the complete acceptance contract. Check constant preservation, the two-pixel `[0,1]` case with `kappa=1, dt=1/4` yielding `[1/8,7/8]`, conductance symmetry, zero steps, oracle-only diagnostics for invalid stability bounds, compiled ROI/tile reads, cancellation and resource failure. See [oracle README](../../../../oracle/ops/filter/README.md).

The proposed native keys are unregistered and unimplemented; dtype-aware native acceptance follows RES_native_contract. No runtime or performance evidence is implied.
