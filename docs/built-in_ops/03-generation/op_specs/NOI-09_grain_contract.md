---
spec_schema_version: 1
id: NOI-09
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# NOI-09: Multiplicative speckle and artistic grain

Integer-looks speckle and linear-RGB grain have different models. Grain is not a
calibrated camera or film model. Its explicit normalization and kernel determine noise
scale; strength is a multiplier. Alpha is copied, including signed zero, and hidden RGB
is processed. Other color domains require separate operators.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-09A](NOI-09A_speckle_integer_looks.md) | speckle integer looks |
| [NOI-09B](NOI-09B_artistic_linear_grain.md) | artistic linear grain |

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
