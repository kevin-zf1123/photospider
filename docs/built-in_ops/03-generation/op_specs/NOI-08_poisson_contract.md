---
spec_schema_version: 1
id: NOI-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-08: Poisson counts and electron shot

The inverse-CDF sequence and electron-unit workflow are separate. Negative expected
electrons fail. Read noise, gain, black level, full well, clipping and quantization are
explicit later operations. Integer branch decisions do not receive accelerated numerical
tolerance.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-08A](NOI-08A_poisson_icdf.md) | poisson icdf |
| [NOI-08B](NOI-08B_shot_electrons.md) | shot electrons |

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
