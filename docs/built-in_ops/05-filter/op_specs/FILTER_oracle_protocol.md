---
spec_schema_version: 1
id: FILTER-oracle-protocol
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Mathematical, quality and runtime acceptance

## Evidence classes

| Class | Establishes | Does not establish |
| --- | --- | --- |
| ExactRational | Declared rational expression/stage with direct IEEE rounding | Full special-value coverage or production ownership |
| DirectedMPFR | Directed interval endpoints round to the same bits | Unbounded completion or all-input certificates |
| HighPrecisionDiagnostic | Approximate high-precision/reference error trends | Strict golden or truncation-error proof |
| ContractFixture | Executable support/schema/control assertions | Runtime implementation |
| PinnedThirdPartyComparison | Actual fixed implementation/config/resource comparison | Unconditional bit identity, quality ground truth or NUM compliance |

Two agreeing precisions do not constitute a certificate. Generated vectors require
independent analytic assertions. Python plane lists do not implement tensor layout,
Result ownership or the compiler. See [oracle coverage](../oracle-coverage.md) and
[execution instructions](../../../../oracle/ops/filter/README.md).

## Mathematical acceptance

Use direct rational-to-binary32/64 rounding, including double-rounding counterexamples.
Use directed MPFR for transcendental expressions; recognize exact algebraic zero
instead of clipping small residuals. DFT/DCT cyclotomic reduction and independent
finite-matrix Wiener/Tikhonov references provide structurally separate checks.
RL tests construct A and its true transpose and verify the adjoint identity.
Validate every declared S rounding boundary, Float32 iterative trajectories, rational
PSF feasibility and direct output rounding. Anscombe mean-domain inverses require
strict certificates before registration; current diagnostic roots are insufficient.

Fixtures cover both dtypes, signed zeros, subnormal/normal boundaries, halfway cases,
overflow to infinity, NaN sign/payload/priority, infinity arithmetic, selected and
excluded poison values, odd/even shapes, large kernels, singleton axes, nonzero
origins and identity branches. Deterministic fixtures record input/expected bits.
Partial finite-domain references explicitly report unsupported cases as Inconclusive,
not as evidence that legal NUM inputs are invalid.

## Native algorithms and third-party comparison

BM3D, CBM3D and SMAA 1x are native algorithms with independently pinned third-party
comparisons. Record version, build, resource identity/license, input representation,
parameters, stages and sampling behavior. Align algorithm steps and explain numerical
differences. Neither a manifest nor a citation supplies actual pixel evidence. Required
resource/profile details must be resolved before a complete executable profile is
claimed. There is no external-engine fallback disguised as native registration.

## Image-quality acceptance

Effect-oriented members require mathematical acceptance and quantitative quality
scores by use case. Use multiple relevant metrics with each critical threshold met;
do not let a weighted total hide failure on a critical metric. Reliable references
from known synthetic degradation or measured pairs form the primary dataset;
unpaired real images supplement it. Third-party output is not ground truth.
Metric formulas, datasets, sample aggregation and thresholds are established during
implementation; this specification fixes the principles, not those values. No
mandatory manual-review gate or universal cross-effect score is implied.

## Public runtime acceptance

| ID | Required evidence |
| --- | --- |
| H01 | Missing/type/domain/shape/axis validation before prohibited reads |
| H02 | Whole versus nonzero ROI/tile crops where mathematically applicable |
| H03 | Exact sparse support, excluded mask/tap data and dynamic radius demand |
| H04 | Control dirty witnesses for guide/mask/kernel/radius changes |
| H05 | Independent/joint outputs and failure scope |
| H06 | Strict bits across parallelism/fenv, gradual underflow and NUM special values |
| H07 | Named ISA accelerated final bounds and strict fallback |
| H08 | Capacity/work/stage/limb limits, cancellation and no spill/degradation |
| H09 | Immutable owners/read windows beyond context lifetime and final release |
| H10 | FMT metadata, straight/alpha, arbitrary group slots, Lab units and AOV |
| H11 | Grid conjugacy, original shape, lazy bands and edited reconstruction |
| H12 | Descriptor/profile/resource/parameter cache identity and early-stop cache exclusion |
| H13 | Actual BM3D/CBM3D/SMAA comparisons with pinned configurations |
| H14 | Same-semantics benchmarks with temporary peaks and certificate costs |
| H15 | Asynchronous stopping state retention, triggers, races and safe cleanup |

These are acceptance requirements, not completed runtime tests. Mathematical oracle
success does not establish registration, backend availability, performance or quality.
