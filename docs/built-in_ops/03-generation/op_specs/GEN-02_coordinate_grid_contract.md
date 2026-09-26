---
spec_schema_version: 1
id: GEN-02
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-02: Coordinate grids

Canvas geometry is explicit; xy components and scalar axis sources are separate. Pixel
centers define identity sampling coordinates, while normalized_edge uses W/H, not
W-1/H-1. The old field.coordinate implementation is retired.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-02A](GEN-02A_coordinates_xy.md) | coordinates xy |
| [GEN-02B](GEN-02B_coordinate_axis.md) | coordinate axis |

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
