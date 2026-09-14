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
| NUM-03 constant / broadcast | Complete (2026-09-14) | Per-node metadata, bounded owned/borrowed views, exact compact mappings and explicit NEON/AVX2 dense copy paths; strict/Apple locally and strict/x86 in Clang WSL passed manual arrays/mappings, bit-pattern, cache, typed-validation, resource and ownership checks; installed consumer and focused compiler/resources/dependency/fragments units passed; independent scoped review required fixes closed |
| NUM-04 unary / trigonometric / rational pi | Pending | |
| NUM-05 binary | Pending | |
| NUM-06 clamp / remap | Complete (2026-09-14) | Six keys; dynamic bounds, raw-bit clamp and whole-formula exact rational rounding; strict/Apple and Clang WSL strict/x86 passed 2826 Fraction cases per profile, explicit broadcast composition, all-port support, InvalidBounds isolation, upstream/typed errors, cache, fenv, layouts, work/cancel and owner cleanup; installed consumer, independent math/entry reviews and lint passed |
| NUM-07 comparison / select | Complete (2026-09-14) | Eight operations / 24 keys; exact IEEE comparisons and dyadic is_close, staged select with precise Control/Data/Validation; strict/Apple and Clang WSL strict/x86 passed 3760 independent Fraction cases per profile and public composition, source/error isolation, bit-pattern, typed-validation, cache, fenv, resource and cleanup checks; installed consumer, test_resources, formatting/lint and independent scoped reviews passed |
| NUM-08 mix / smoothstep | Complete (2026-09-14) | Six keys; staged factor/branch reads and whole-formula exact dyadic/cubic rational rounding with bounded host-owned scratch; local strict/Apple and Clang WSL strict/x86 passed 5242 Fraction cases per profile, public broadcast/smoothstep/mix composition, exact roles/support, atom errors, typed closure, factor-cache replacement, layouts/ROI, fenv, WorkLimit/cancel and cleanup; installed consumer, NUM-06/07 arithmetic regressions, independent math/entry reviews and lint passed |
| NUM-09 reshape / transpose / slice | Complete (2026-09-14) | Nine keys; per-rectangle view/auto/dense, exact staged or compact mappings, conditional payload admission and retained metadata owners; local strict/Apple and Clang WSL strict/AVX2 passed 636 independent coordinate/raw-bit cases per profile plus public composition, owner fragmentation, strides, typed closures, sparse dirty support, schema/Empty, work/cancel/capacity, fenv and escaped lifetime checks; installed consumer, focused compiler/dependency/fragments/resources units and formatting/lint passed; scoped review closed required fixes; documented legacy host-container metadata boundary |
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
