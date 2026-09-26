---
spec_schema_version: 1
id: PTH-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-02: Path evaluation

Outputs are position[N,2], derivative[N,2], tangent[N,2] and UInt8 tangent_valid[N].
Derivatives are with respect to local normalized t. Exact zero derivative yields
(+0,+0),valid0; otherwise tangent=d/sqrt(dot(d,d)). Requested position alone need not
compute the tangent. Shared schema/control validation remains required.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-02A](PTH-02A_evaluate_bezier_path.md) | evaluate bezier path |
| [PTH-02B](PTH-02B_evaluate_elliptic_arc.md) | evaluate elliptic arc |
| [PTH-02C](PTH-02C_evaluate_hermite_path.md) | evaluate hermite path |
| [PTH-02D](PTH-02D_evaluate_bspline_path.md) | evaluate bspline path |
| [PTH-02E](PTH-02E_evaluate_pathset.md) | evaluate pathset |

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
