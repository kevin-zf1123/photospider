---
spec_schema_version: 1
id: PTH-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-08: Simplification, fitting and smoothing

Point reduction, curve fitting and deliberate shape change have separate contracts.
Fitting backends are distinct versioned operators with a shared continuous-error
acceptance rule, not interchangeable solver modes. Approximation/fitting defaults to
allow_change; reject_unproven needs the declared topology proof. Report unverified
honestly. Chaikin is explicitly shape-changing.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-08A](PTH-08A_simplify_polyline.md) | simplify polyline |
| [PTH-08B](PTH-08B_fit_cubic_segments.md) | fit cubic segments |
| [PTH-08C](PTH-08C_smooth_chaikin.md) | smooth chaikin |

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
