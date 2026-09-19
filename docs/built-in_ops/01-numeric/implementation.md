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
| NUM-10 concatenate / gather / scatter | Complete (2026-09-14) | Eighteen keys; translated disjoint static pieces, hit-only concatenate views, observed-index gather and globally validated scatter with exact contributors/aggregates; local strict/Apple and Clang WSL strict/AVX2 passed 3858 independent coordinate/Fraction cases per profile and public workflows, typed/stride/fenv/cache/work/cancel/failure-diagnostic checks; installed consumer, focused compiler/dependency/fragments/resources units, formatting/lint and independent scoped reviews passed |
| NUM-11 reductions | Complete (2026-09-19) | Twenty-one keys; 64-value streamed exact sum/min/max/mean/moments, directly rounded exact standard deviation and metadata-only count; local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 4740 independent Fraction/midpoint-square cases per profile plus public workflows, 4096-element/16 KiB streaming, giant count/8-byte owner, sparse support/dirty, atom overflow, typed/Empty/cancel/ddof/strided-NaN and upstream-failure checks; installed consumer, test_compiler, NUM-08/10 arithmetic and manual regressions, formatting/lint and independent math/entry reviews passed |
| NUM-12 sort / quantile | Complete (2026-09-19) | Six keys; stable raw-bit sorting and exact UInt128/Fraction quantile, with optional accounted cross-output permutation reuse through new C++ traits14/package0.14; local Clang strict/Apple and Ubuntu WSL Clang strict/AVX2 passed 2072 independent cases per profile plus multi-output/sparse/typed/fenv/work/cancel/cache and failure-isolation workflows; installed consumer and old-minor rejection, focused compiler/dependency/resources units, formatting/lint and scoped math/entry/block-identity reviews passed |
| NUM-13 prefix / integral image | Complete (2026-09-19) | Six keys; bounded exact carry and conversion snapshot, per-line regional prefixes and independently streamed rectangles; local Clang strict/Apple and WSL Clang strict/AVX2 passed 2280 independent cases per profile and public fixtures, sparse/L-shaped support, Atom overflow, typed/Empty/zero reads, strides/fenv, output cap, cancellation/work/release, and 4096-value/65-window/16 KiB scan checks; installed consumers, focused compiler unit, formatting/lint and scoped math/entry reviews passed; dense association-work limitation documented |
| NUM-14 matrix transform | Complete (2026-09-19) | Three keys; 4352-bit exact dot plus bias with one final rounding, sparse per-component vector/row/bias associations and deduplicated transport; local Clang strict/Apple and WSL Clang strict/AVX2 passed 1110 independent cases per profile, public affine fixtures, sparse support/dirty, typed/Empty/schema, cache/lifetime, all-port negative strides/fenv, WorkLimit/cancel/release and NaN upstream-failure checks; installed consumers, focused compiler unit, formatting/lint and independent math/entry review passed |
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
