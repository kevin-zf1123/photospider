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
- restoration.diffusion_exponential
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-06A: Exponential conductance diffusion, fixed steps

Inherit [RES-06](RES-06_contract.md), the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation key: `restoration.diffusion_exponential`.

### Ports

`input` and `output` are Results with matching fixed Tensor schemas. Each plane is an independent raw numerical field; channel count does not imply RGB/vector/alpha behavior. Downstream color interpretation, if needed, is explicit caller-supplied configuration.

### Parameters

Require finite `kappa>0`, `dt>0`, `max_iterations>=0`, finite `dx>0`, finite `dy>0`, and explicit `y_axis` and `x_axis`. Require `dt <= 1/(2*(1/dx^2+1/dy^2))`. Boundary is the fixed no-flux definition. All caller controls must be supplied; constructors do not add defaults. Values use the actual stored Float64 parameter values. No normalization is inferred from dtype or image range.

## Mathematics

For each complete synchronous iteration and each undirected in-domain edge `(i,j)`, let `d=(u_j-u_i)/spacing_ij` and compute one native input-dtype conductance `c_ij=exp(-(d/kappa)^2)`. Use that same coefficient in both edge directions and update `u_new[i]=RN_t(u_i+dt*Σ_j c_ij*(u_j-u_i)/spacing_ij^2)`. States use main input dtype `t`; do not calculate all iterations in a separate Float64 state. At zero iterations return input bit-copy. No external boundary flux is added. Do not claim bitwise mass conservation; stage rounding may cause small drift.

The profile is F: native state and conductance use the main input dtype, while synchronous iteration boundaries remain normative. The finite parameter/control-model and stability domains are caller premises; do not blanket-reject propagated NaN/Inf under the native arithmetic contract. Do not clamp output.
