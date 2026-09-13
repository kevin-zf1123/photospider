# 01-numeric implementation work

Scope: all NUM-01..15 and CRV-01..11 contracts in this directory, based on
`ops@0c567b6d`, implemented on `ops-impl`. Primitive keys and authoring templates
retain the distinctions in their individual specifications. Specification
acceptance and implementation completion remain separate states.

Manual executable acceptance is kept outside CTest and the integration-test
inventory. Each completed functional cluster receives its own commit. A new
independent agent performs the final comprehensive review after all items below
are complete; required fixes precede the branch push.

| Item | Status | Implementation / acceptance |
| --- | --- | --- |
| Shared numeric, metadata, observation, profile and resource facilities | In progress | Bounded exact sequence arithmetic, axis dtype inference, tuple observations and numeric diagnostics implemented; later-family shape/view and numerical facilities remain |
| NUM-01 expression | Pending | Spec and shared numeric facilities |
| NUM-02 linspace / arange | Complete (2026-09-14) | Clang strict/NEON locally and strict/AVX2 in Ubuntu WSL passed public workflows and 960 independent rational/IEEE cases per profile on 2026-09-14; installed consumer passed locally; independent scoped review fixes validated; test_compiler/test_resources and ClangFormat 21/cpplint passed |
| NUM-03 constant / broadcast | Pending | |
| NUM-04 unary / trigonometric / rational pi | Pending | |
| NUM-05 binary | Pending | |
| NUM-06 clamp / remap | Pending | |
| NUM-07 comparison / select | Pending | |
| NUM-08 mix / smoothstep | Pending | |
| NUM-09 reshape / transpose / slice | Pending | |
| NUM-10 concatenate / gather / scatter | Pending | |
| NUM-11 reductions | Pending | |
| NUM-12 sort / quantile | Pending | |
| NUM-13 prefix / integral image | Pending | |
| NUM-14 matrix transform | Pending | |
| NUM-15 derivative / integrate | Pending | |
| CRV-01 interpolation | Pending | |
| CRV-02 Bézier function sampling | Pending | |
| CRV-03 parametric Bézier evaluation | Pending | |
| CRV-04 LUT1D authoring templates | Pending | |
| CRV-05 LUT1D application | Pending | |
| CRV-06 ColorArray / color ramps / retained ICC resource | Pending | |
| CRV-07 LUT3D application | Pending | |
| CRV-08 shapers | Pending | |
| CRV-09 LUT3D baking and measured report template | Pending | |
| CRV-10 inverse curves | Pending | |
| CRV-11 resampling templates / uniform and nonuniform lowpass | Pending | |
| Final independent comprehensive review and required fixes | Pending | Starts after all functional items |
| Push reviewed branch | Pending | |

Current manual entry: [numeric workflow](../../../examples/numeric_workflow/README.md).
Individual completed clusters do not imply category completion. No performance claim is inferred from correctness measurements.
