---
spec_schema_version: 1
id: PTH-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-02: Path evaluation

Outputs are position[N,2], derivative[N,2], tangent[N,2] and UInt8 tangent_valid[N].
Derivatives are with respect to local normalized t. Exact zero derivative yields
(+0,+0),valid0; otherwise tangent=d/sqrt(dot(d,d)). Requested position alone need not
compute the tangent. Only static schema checks remain; controls and indices are unchecked caller premises.

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
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The listed members are implemented through fixed invocation ABI 8 and the public generation SDK. Unresolved numerical solver or backend choices remain explicit member limitations.
