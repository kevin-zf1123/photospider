---
spec_schema_version: 1
id: NOI-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-06: Fractal sums

Bases are versioned member mathematics, not opaque callbacks. Unrounded basis values and
exact octave coordinates participate in the final expression. Octaves share scaled
samples of the chosen base field; independence is not claimed. Periodicity with
lacunarity requires proof.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-06A](NOI-06A_fbm_sum.md) | fbm sum |
| [NOI-06B](NOI-06B_turbulence_sum.md) | turbulence sum |
| [NOI-06C](NOI-06C_ridged_sum.md) | ridged sum |
| [NOI-06D](NOI-06D_footprint_fbm.md) | footprint fbm |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or Whole from actual mathematical dependencies,
independently of complete control validation. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Public Result data requires validated source
associations and lifetime.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. Concrete schema, RNG mapping and
backend/solver details identified by the member must be specified and validated before
registering that member.
