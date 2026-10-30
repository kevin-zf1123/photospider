---
spec_schema_version: 1
id: PTH-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-08: Simplification, fitting and smoothing

Point reduction, curve fitting and deliberate shape change have separate contracts. Fitting backends are distinct versioned operators with a shared continuous-error acceptance rule, not interchangeable solver modes. Approximation/fitting defaults to allow_change; reject_unproven needs the declared topology proof. Report unverified honestly. Chaikin is explicitly shape-changing.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-08A](PTH-08A_simplify_polyline.md) | simplify polyline |
| [PTH-08B](PTH-08B_fit_cubic_segments.md) | fit cubic segments |
| [PTH-08C](PTH-08C_smooth_chaikin.md) | smooth chaikin |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
