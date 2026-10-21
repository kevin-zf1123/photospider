---
spec_schema_version: 1
id: GEN-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-06: Spread and gradient lookup

Spread, uniform raw-table lookup and model-specific color workflows are distinct.
Preserve each public node rounding stage. Color ramps retain original hue and winding,
including achromatic input; no implicit shortest-path selection or hue normalization.
Model-specific rules inherit current CRV/FMT contracts.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-06A](GEN-06A_spread_coordinate.md) | spread coordinate |
| [GEN-06B](GEN-06B_lookup_numeric_gradient.md) | lookup numeric gradient |
| [GEN-06C](GEN-06C_gradient_color.md) | gradient color |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The specified schemas and RNG mappings require implementation and validation before registration. Unresolved numerical solver or backend choices remain explicit member limitations.
