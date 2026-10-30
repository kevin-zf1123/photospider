# RES01 native implementation and performance record

Measured on 2026-10-29. This record covers the two implemented RES01 operators and the RES01–12 arithmetic-specification migration. Specifications remain Proposed. RES02–12 are not claimed implemented by this change.

## Implemented interface

| Member | Registered key | Inputs | Selected outputs |
|---|---|---|---|
| RES-01A | `restoration.noise_mad_highpass` | Float32/Float64 tensor and same-shape UInt8 mask | Float64 `sigma`, one scalar per nonspatial plane |
| RES-01B | `restoration.calibrated_noise_variance` | Float32/Float64 `mean_signal` | `variance`, `sigma`, or both, in the input dtype and shape |

A requires explicit Int64 `y_axis`, `x_axis`, and `min_samples`. The two axes must be distinct. Rank 2–8 is supported; nonspatial dimensions retain their order and a terminal extent 1 is appended to the output shape. Logical batch dimensions participate in that rule. Mask values are caller-supplied 0/1; a patch participates only when all four mask values are one. Unselected image samples do not enter arithmetic. Fewer than `min_samples` selected patches is `OperationFailed/InvalidDomain`.

B requires explicit Float64 `gain`, `offset`, `read_sigma` and String `below_offset` (`clamp` or `error`). Real parameters are converted to the working dtype. `error` describes the caller's below-offset domain premise, not a request for a payload-validation pass. The native evaluation is `delta=max(input-offset,0)`, `read2=read_sigma*read_sigma`, `variance=fma(gain,delta,read2)`, then `sigma=sqrt(variance)`. A sigma-only request keeps variance in registers without publishing its image.

Use these keys through existing `WorkflowNode` registration; output references use the strings `sigma` and `variance`. There is no new kernel API or ABI layout. The operation module ABI 8 and native service ABI 2 remain unchanged; trusted C++ registration opts into the already available fixed-invocation v9 work service, following the FFT implementation.

Implementation files are [runtime integration](../../../plugins/ops/05-filter/restoration_noise.cpp) and [native numerical kernels](../../../plugins/ops/05-filter/restoration_noise_math.hpp). Registration changes are confined to the bundled operation registry and its source list.

## Specification review

All 34 members in RES01–12 now refer to [the shared native contract](op_specs/RES_native_contract.md), with one unsuffixed key per implemented primitive, native fp32/fp64 working state, and dtype-aware numerical acceptance. The migration removes inherited NUM four-ULP quality tiers, exact whole-expression rounding and arbitrary-precision production fallback. It retains explicitly Float64 outputs, native exponent ranges, support, masks, exact copies, rank selection, iteration boundaries and resource ownership.

The review also preserves distinctions that a mechanical text replacement would lose:

- RES09 uses the same native normalized coefficients for finite A and its true transpose. Structurally unobserved support is determined from coefficients and mask, not from a rounded sensitivity zero.
- RES10 keeps the sorting-and-threshold simplex projection. Native PSF state replaces rational shadow state; nonnegativity/support zeros remain exact and unit mass has a documented roundoff allowance. There is no extra normalization step.
- RES12 keeps the interface, coefficient and conversion contracts of existing FIL/FRQ helpers.
- RES04's unresolved comparison/profile gates and RES07's remaining mean-inverse reference gaps remain explicit.

RES13/14 and frozen kernel files are outside this change. Existing mathematical oracles for unimplemented members remain references, not native bitwise goldens. RES01's Python entry points now independently model the native stages; the former formulas remain named reference helpers.

## Benchmark scope and complete matrix

The reproducible driver is [tools/res_performance](../../../tools/res_performance/README.md). It includes the production numerical header, with no substituted algorithm. Each invocation recomputes the result. Both candidates use the same empty compiler barrier with escaped buffer pointers after every call, preventing dead-store elimination and loop hoisting; actual timed-loop callsites are also inspected. Input/output buffers are reused; allocation, fixture initialization, scratch first touch and first numerical call are reported separately. The executor is measured separately below. MATLAB measurements are public expressions including temporary arrays; they are not isolated measurements of MATLAB's internal kernels.

Each binary covers fp32 and fp64 at square sides `2^p + d`, for `p={4,5,8,10,12,14}` and `d={-1,0,1}`, plus `1x65536` and portrait A4 at 150/300/350/600 dpi. A4 shapes, as height × width, are 1754×1240, 3508×2480, 4093×2894 and 7016×4961. There are 23 shapes per dtype.

A has no valid 2×2 patch on `1x65536`. Both dtypes are checked for the specified insufficient-samples outcome; these two cases have no numerical speedup ratio. The eligible matrix contains 44 MAD cases and 46 cases for each B output mode (variance-only, sigma-only, joint), **182 comparisons per executable**. All-ones masks are used for performance; sparse masks, masked poison, rank/axis/batch layouts and runtime behavior are separate acceptance checks.

The deterministic input hashes each row-major uint32 index into [-2,8). OpenCV and Photospider alternate in timed blocks targeting about 20 ms; the median of 3, 5 or 9 rounds is retained according to size. Both use one thread. MATLAB also uses one computational thread. Linux processes are pinned to logical CPU 8, a P-core of the measured i9-12900. Performance jobs run serially on each host, and profiler timings do not enter the matrix.

| Host | Compiler | OpenCV | MATLAB |
|---|---|---|---|
| M5 MacBook Pro, 32 GiB, macOS 27.2 | Apple Clang 21.0.0 | 4.12.0 | R2024a, 24.1.0.2537033 |
| Intel i9-12900, 64 GiB, Linux | Clang 21.1.8; icpx 2026.2.0 | 4.10.0 | R2026b, 26.2.0.3386108 |

Both optimization levels use `-std=c++17`, `-g`, `-fno-fast-math`, `-frounding-math`, and `-ffp-contract=off`. Explicit FMA intrinsics and `std::fma` remain permitted. x86 packet functions request AVX2/FMA and check ISA availability; no global `-march=native` requirement is introduced. ARM uses native NEON packets. `build.py` records the exact command, compiler version and executable/source SHA-256 hashes.

### Baselines

OpenCV A composes four-mask intersection, valid 2×2 arithmetic, two exact medians and absolute deviations. The medians use `std::nth_element` plus the lower-middle maximum, not full sorting; this makes the composed reference stronger than a `cv::sort` implementation. OpenCV B composes `subtract`, `max`, affine `convertTo` and `sqrt`, omitting unrequested output work and reusing preallocated buffers.

MATLAB A tries indexed native residuals, two `diff` operations, and valid `conv2`, followed by native `median`, absolute deviations, another median and a Float64 scale conversion. The installed sessions do not have Statistics Toolbox `mad`. MATLAB B tries the ordinary affine expression and `fma` where the actual installed function accepts the dtype. M5 has only the ordinary expression; Linux has both. The fastest verified native-consistent candidate is selected per baseline case. All three MAD forms use the same valid four-tap support and exact native median/MAD; their grouping can change fp32 residuals. The summary checks the shared RES01A scale-aware native envelope using this fixture's |input|<=8, rather than applying Float64 epsilon to the earlier fp32 stages. The much tighter Float64 tolerance applies only to the final scaling of a given native MAD. Original timings and chosen method names are retained; this final eligibility clarification re-summarizes existing MATLAB measurements and does not pretend they were rerun.

The MATLAB variant check and C++ B benchmark `error` field are sampled results, not exhaustive array certification. Independent oracle and public-runtime checks provide array acceptance. A's benchmark result is its complete scalar output. Scalar agreement alone does not establish rank correctness; the composed methods use exact medians by construction, and the production selector receives independent rank/adversarial checks.

### Whole-build results

All six complete numerical builds pass against both baselines. Each cell uses a single Photospider executable; MATLAB selects its fastest eligible candidate separately for each case. No tested MATLAB candidate is excluded by the final native acceptance check.

| Build | OpenCV minimum | OpenCV geometric mean | OpenCV ≥2× | MATLAB minimum | MATLAB geometric mean | MATLAB ≥2× |
|---|---:|---:|---:|---:|---:|---:|
| M5 Clang O2 | 1.539× | 4.965× | 176/182 (96.7%) | 1.856× | 5.685× | 175/182 (96.2%) |
| M5 Clang O3 | 1.559× | 5.007× | 177/182 (97.3%) | 1.878× | 5.646× | 175/182 (96.2%) |
| i9 Clang O2 | 1.954× | 5.513× | 179/182 (98.4%) | 2.046× | 7.499× | 182/182 (100.0%) |
| i9 Clang O3 | 1.943× | 5.512× | 179/182 (98.4%) | 2.066× | 7.490× | 182/182 (100.0%) |
| i9 icpx O2 | 1.946× | 5.480× | 180/182 (98.9%) | 2.064× | 7.522× | 182/182 (100.0%) |
| i9 icpx O3 | 1.940× | 5.503× | 181/182 (99.5%) | 2.049× | 7.536× | 182/182 (100.0%) |

Every build has **182/182 cases ≥1.5× for each baseline**. These are 1,092 complete numerical cases across the six builds, with two baseline comparisons per case. The weakest observed margin is M5 Clang O2 at 1.539× against OpenCV. This is an observed result on the recorded machines, not a guarantee for other CPUs, layouts, masks or data distributions.

The following operation-level breakdown aggregates fp32 and fp64 within each build. All dtype/shape timings, chosen MATLAB methods and ratios remain in `summary/cases.csv`; `summary/summary.json` also contains a separate operation-by-dtype breakdown.

| Build | Output mode | Cases | OpenCV minimum / ≥2× count | MATLAB minimum / ≥2× count |
|---|---|---:|---:|---:|
| M5 Clang O2 | mad | 44 | 1.539× / 38/44 | 2.868× / 44/44 |
| M5 Clang O2 | variance | 46 | 3.489× / 46/46 | 1.856× / 39/46 |
| M5 Clang O2 | sigma | 46 | 3.071× / 46/46 | 3.282× / 46/46 |
| M5 Clang O2 | joint | 46 | 2.741× / 46/46 | 2.808× / 46/46 |
| M5 Clang O3 | mad | 44 | 1.559× / 39/44 | 2.894× / 44/44 |
| M5 Clang O3 | variance | 46 | 3.502× / 46/46 | 1.878× / 39/46 |
| M5 Clang O3 | sigma | 46 | 3.059× / 46/46 | 3.284× / 46/46 |
| M5 Clang O3 | joint | 46 | 2.741× / 46/46 | 2.874× / 46/46 |
| i9 Clang O2 | mad | 44 | 2.395× / 44/44 | 5.218× / 44/44 |
| i9 Clang O2 | variance | 46 | 3.780× / 46/46 | 2.046× / 46/46 |
| i9 Clang O2 | sigma | 46 | 1.954× / 43/46 | 2.674× / 46/46 |
| i9 Clang O2 | joint | 46 | 2.102× / 46/46 | 2.678× / 46/46 |
| i9 Clang O3 | mad | 44 | 2.322× / 44/44 | 5.263× / 44/44 |
| i9 Clang O3 | variance | 46 | 4.155× / 46/46 | 2.066× / 46/46 |
| i9 Clang O3 | sigma | 46 | 1.943× / 43/46 | 2.679× / 46/46 |
| i9 Clang O3 | joint | 46 | 2.092× / 46/46 | 2.680× / 46/46 |
| i9 icpx O2 | mad | 44 | 2.374× / 44/44 | 5.813× / 44/44 |
| i9 icpx O2 | variance | 46 | 3.767× / 46/46 | 2.064× / 46/46 |
| i9 icpx O2 | sigma | 46 | 1.946× / 44/46 | 2.680× / 46/46 |
| i9 icpx O2 | joint | 46 | 2.085× / 46/46 | 2.680× / 46/46 |
| i9 icpx O3 | mad | 44 | 2.305× / 44/44 | 5.717× / 44/44 |
| i9 icpx O3 | variance | 46 | 3.941× / 46/46 | 2.049× / 46/46 |
| i9 icpx O3 | sigma | 46 | 1.940× / 45/46 | 2.673× / 46/46 |
| i9 icpx O3 | joint | 46 | 2.119× / 46/46 | 2.673× / 46/46 |

A configuration passes the requested numerical threshold only if every eligible comparison is at least 1.5× and strictly more than half are at least 2×. No row-wise selection between Photospider executables is used. Raw first-pass matrices are retained even when a later source revision fixes their regressions. Runs before the explicit compiler barrier are optimization diagnostics; final threshold claims use only the complete guarded `accepted` matrices.

### O2/O3 and compiler comparisons

Ratios below use Photospider time(left)/time(right) for all 182 identical cases. A value above 1 means the right-hand configuration was faster. These are whole-matrix descriptive statistics, not significance tests.

| Comparison | Geometric mean | Median |
|---|---:|---:|
| M5 O2 / O3 | 0.99309 | 0.99645 |
| Intel clang O2 / O3 | 0.99888 | 1.00013 |
| Intel icpx O2 / O3 | 1.00182 | 1.00012 |
| Intel icpx O2 / clang O2 | 0.99690 | 0.99995 |
| Intel icpx O3 / clang O3 | 0.99397 | 1.00002 |

All measured whole-matrix compiler/optimization differences are below 1% in geometric mean. Individual cases vary more (the complete per-operation ranges are retained), but neither O3 nor icpx provides a universal advantage. No global build-flag change is justified by these runs; algorithmic selection, pass fusion and store policy produced the material gains.

## Algorithm and measured hotspots

### A: exact sample-guided selection

The high-pass residual and absolute-deviation passes use the input dtype. A full sort is unnecessary because only the middle rank(s) are needed. A small regularly spaced sample chooses a bracket; a SIMD pass counts values below it and compacts its contents. The implementation checks that both required ranks are actually retained. A failed bracket falls back to exact full-range selection. Equal bracket endpoints handle repeated values without an approximate quantile.

Large inputs use 65 or 513 samples, followed by bounded recursive narrowing of candidates already in scratch. Small 512–4095-element ranges use a 33-sample bracket; smaller ranges use the standard selector. This small-range path fixed the observed 33×33 Intel regression. Sampling determines work, never the returned rank. The implementation is informed by two-pivot selection research; it is not a claim to implement every detail or prove the complexity bound of Floyd–Rivest SELECT.

The first M5 profile put 3,342 of 4,241 samples in scalar bracket selection and 792 in the remaining `nth_element` path. Replacing scalar compaction with packet comparisons and shuffle-table stores, then reducing the retained set before the final selector, addressed those observed costs. The guarded 1024² fp32 numerical profile has 2,323 samples: 1,686 at the main selector, 334 at candidate refinement, 207 at masked residual generation and six at the remaining sorting path; 90 are attributed to the enclosing driver. Function percentages are relative cost, not before/after wall-time speedups.

Compaction tables are `constexpr`, not lazily initialized at each call. NEON uses table shuffles; AVX2 uses comparison masks, `popcnt`, a permutation table and full packet stores with explicitly reserved padding. The candidate buffers alternate inside the existing scratch allocation. No data-dependent result is cached across calls.

### B: one pass and output specialization

The subtraction, clamp, FMA and optional square root execute in one streaming loop. Selecting variance/sigma/joint happens before the packet loop through compile-time specializations. Four independent packets are unrolled to expose load/FMA/square-root overlap, and joint output reuses each native variance value for sigma. This removes full-image intermediate passes and repeated output-pointer branches from the hot loop.

The inspected M5 binary uses `fsub.4s/2d`, `fmax.4s/2d`, `fmla.4s/2d`, and `fsqrt.4s/2d`, with `ldp/stp` packet accesses. Its joint loop contains only loop control; optional-output branches are outside it. The guarded joint numerical profile places 2,311 of 2,312 samples in `variance<double>`. There is no approximate reciprocal-square-root shortcut.

Intel joint output exposed a second bottleneck that varied across small-array runs and affected both compilers. The actual AVX2 body already loaded four packets before stores, but all three benchmark arrays had a 16-byte page offset. A common 32-byte alignment prefix removed split accesses from the full packet loop, yet five-process repetitions still had slow cases; alignment alone was not retained for joint output. Explicit non-temporal output stores stabilized fp32 joint at 255²/256²/257² to 9.67–9.86 microseconds over five repetitions, compared with 15.16–28.97 microseconds for the preceding temporal implementation. Both versions used the guarded driver, complete outputs, and a fence on every streaming numerical call.

In the final separate 100,000-call fp32 255² run, affinity is set before starting `perf`, and only P-core events are collected. Cycles changed from 7,506,384,099 to 4,943,358,791, while retired instructions changed only from 6,758,126,165 to 6,797,425,986. `RESOURCE_STALLS.SB` fell from 6,363,502,988 to 26,486,441 cycles (84.8% to 0.54% of measured core cycles). `LD_BLOCKS.ADDRESS_ALIAS` counted only 586 versus 643 events, and store-forward blocking was 7,240 versus 7,097 events. In a separate event group, retired split loads fell from 406,405,870 to 5,259, and split stores from 812,804,934 to 4,568; the streamed path's aligned full-line body removes those splits.

These measurements support store-buffer pressure and split-access removal as the resolved costs; they do not support the earlier 4 KiB-alias hypothesis. Stall cycles and blocking-event counts are different units and are not added together. The split-event group is a separate process run. Generic cache misses are not used as a calibrated DRAM-byte measurement. Raw counters are in `accepted-joint-{base,streamed}-{sb,split}.txt`.

The final i9 Clang O3 numerical profiles put 59.90% of MAD samples in bracket/compaction, 18.28% in deviations, 15.59% in packet residual generation and 4.98% in mask/residual traversal. Remaining exact selection/sort functions are each below 0.4%. Joint fp64 spends 98.95% in the streamed packet kernel; variance-only fp64 spends 99.03% there. Inlined instruction annotations are approximate sampled locations, not independent additive costs. Only flat symbol/instruction-pointer attribution is used; unresolved call-chain addresses are not interpreted. The generic stat logs include tiny scaled E-core initialization coverage; only their P-core rows are interpreted.

Actual ELF disassembly for Clang O2/O3 and icpx O2/O3 confirms AVX2 loads, `vsubps/pd`, `vmaxps/pd`, explicit `vfmadd213ps/pd`, hardware `vsqrtps/pd`, and both joint output streams using `vmovntps/pd`. Each main group processes four packets with no optional-output branches inside the group. Scalar tails retain scalar square roots. Selection emits compare masks, `popcnt` and permutation-based compaction. Standalone calls include `sfence` after streaming stores. M5 Mach-O disassembly confirms the corresponding NEON vector loops and table shuffles.

The final executable hashes and the three numerical-source hashes match all six build metadata files. `asm-audit.txt` preserves decoded loop bodies and timer-loop evidence; `binary-audit.json` records hash checks. Representative actual fp32 timed-loop call/backedges are:

| Build | Candidate call | Backedge |
|---|---|---|
| i9 Clang O2 | `0x726c` | `0x72a7` |
| i9 Clang O3 | `0x738c` | `0x73c7` |
| i9 icpx O2 | `0x407343` | `0x407352` |
| i9 icpx O3 | `0x407373` | `0x407382` |
| M5 Clang O2 | `0x0000000100004e60` | `0x0000000100004ec0` |
| M5 Clang O3 | `0x000000010000506c` | `0x00000001000050cc` |

Each shown backward branch encloses a call to the production numerical function (or the icpx lambda containing that function), the output observation and sink store. The timer read follows the loop. Both fp32 and fp64 paths were checked. This is evidence from the actual timed executables, beyond merely finding SIMD in an unrelated profile loop.

Eight-packet unrolling, software prefetch and the tested offset-folding variant did not yield a stable improvement and were not retained. Common alignment alone did not resolve the joint-output regression. The desired vector instructions are present in the actual executables. Handwritten assembly was therefore not required; the retained implementation uses intrinsics and compiler-generated packet loops, with scalar tails and runtime ISA dispatch.

### Cache, memory and initialization

Let R=(H−1)(W−1) and T be the input scalar type. A reserves `(2*R+16)*sizeof(T)` bytes per task: one residual plane, candidate scratch and packet padding. Multiple independent output planes within a task reuse that storage sequentially. Neighboring residual rows reuse source rows through the cache; selection/deviation passes stream through the residual plane, while progressively smaller candidate sets fit in smaller caches. This is O(R) scratch, with no unbounded allocation during the numerical callback.

On Intel, variance-only output uses non-temporal stores at 2 MiB of output; joint output uses them at 128 KiB per output when the two output addresses have the same cache-line phase. A scalar prefix aligns the first streamed address to 64 bytes. Each streamed group writes complete 128-byte spans; ordinary tails begin on a new cache line. Mismatched joint-output phases use the ordinary path. These measured thresholds address this host and output pattern, not every downstream pipeline: a consumer that immediately rereads a small output can benefit more from cached stores.

The public runtime adds a **32 MiB per-output minimum** before requesting deferred streaming, because newly allocated result pages behaved differently from the reused numerical buffers. The public-path evidence and threshold checks are below. Each standalone streaming call includes `sfence`. Runtime spans on one invocation thread defer that store-ordering fence until the task guard is destroyed, on success or exception, before publication or allocation release. The fence is not described as a full instruction/timer serialization or a persistence guarantee. Prefix/tail handling prevents adjacent 128-sample spans from mixing temporal and non-temporal stores within the same cache line.

B has no full-image intermediate. Its generic layout fallback reserves only `3*128*sizeof(T)` bytes (1.5 KiB for fp32 or 3 KiB for fp64), reduced from the initial fixed 12 KiB reservation. Compatible contiguous views use the input and output allocations directly. Variance-only or sigma-only performs approximately one input read and one output write; joint output reads once and writes twice. The composed baselines perform additional passes.

Representative fp64 setup and first-use measurements are below, in milliseconds. Allocation is the driver's input/two-output/mask allocation call time; it does not imply that pages are resident. Fixture initialization fills the input and mask and first-touches both outputs. The MAD scratch field includes allocation of scratch and the OpenCV median buffer plus zero first-touch of Photospider scratch; the OpenCV buffer is not first-touched in that field. These costs are not added to reused-buffer speedup ratios.

| Build / shape | Allocation | Fixture initialization | MAD scratch setup | MAD first / reused | Joint first / reused |
|---|---:|---:|---:|---:|---:|
| M5 Clang O2 / 1024×1024 | 0.005 | 2.129 | 0.966 | 2.414 / 2.301 | 0.308 / 0.261 |
| M5 Clang O2 / 7016×4961 | 0.005 | 72.466 | 27.970 | 80.200 / 80.165 | 9.423 / 9.170 |
| M5 Clang O2 / 16385×16385 | 0.045 | 555.593 | 1000.852 | 1095.780 / 596.165 | 68.982 / 70.636 |
| i9 Clang O3 / 1024×1024 | 0.007 | 5.678 | 5.428 | 1.598 / 1.350 | 0.624 / 0.620 |
| i9 Clang O3 / 7016×4961 | 0.008 | 193.722 | 105.615 | 79.712 / 79.894 | 20.794 / 20.672 |
| i9 Clang O3 / 16385×16385 | 0.008 | 1495.188 | 821.790 | 611.406 / 611.161 | 160.219 / 160.298 |

At 16385², one fp64 image is 2,147,745,800 bytes and MAD scratch is 4,294,967,424 bytes (**4 GiB + 128 B**). Initial page population can dominate allocation-call time and may still affect the first numerical pass. The recorded maximum-size public calls below exercise actual managed allocations and publication, rather than assuming that the numerical harness proves those paths.

There is no numerical file or message serialization in either RES01 kernel. Existing schema/plan construction and binding costs belong to the public-call measurements. Fixture generation, binary transfer, JSON output and MATLAB process startup are outside numerical timing. The implementation avoids a new input-packing stage for B, avoids a variance image for sigma-only, and reuses the MAD workspace and prepared metadata; it does not claim to optimize unrelated I/O.

## Public runtime and resource behavior

A retained task owns a cloned immutable program with its selected output region. Its inner prepared kernel is released before that program is destroyed. The v9 wrapper restores the inner context while preserving the Run work service. B groups matching variance/sigma output tiles into one task so jointly requested values share arithmetic; output allocations remain independent. A declares full spatial support for the selected nonspatial planes. ROI/tile edges do not become mathematical image boundaries.

Numerical work is charged to both the invocation limit and the existing Run/Context service. M5 public-runtime sampling found budget/cancellation service and mutex costs alongside selection. Small post-computation input-block charges are combined with a 384-unit flush threshold; a block charges at most 640 units, so these intervals remain below 1024. Selector callbacks retain their own checkpoints because some prepay scan blocks. This changes accounting frequency without changing the total requested work or delaying result checks past publication. B instead prepays the selected region's deterministic six-work-unit-per-sample bound once, using checked 128-bit capacity arithmetic, and polls cancellation independently before each span of at most 128 samples and at completion. Cumulative Run/Context limits remain effective; prepayment is not refunded after cancellation. Coordinator work is additional to those numerical units. There is no numerical refinement stage requirement for native B.

The guarded M5 public-joint profile no longer places the per-span budget service among the leading samples: its main active costs are the numerical loop (399 samples), invocation traversal (347), allocator zeroing (155), view lookup (134) and cancellation (31). The public-MAD profile still includes selection, budget service and mutex work. Idle worker condition-variable samples are reported separately and are not treated as CPU hotspots. Kernel-owned scheduling/allocation is not changed by this operation patch.

### Fresh result pages and the runtime store policy

A final public-call profile exposed a cost hidden by already resident numerical buffers. In 2,000 independent 1024² fp32 joint submissions, the original streamed runtime used 4.803 seconds of task clock and 10.122 billion user core cycles. A temporal-store experiment used 3.218 seconds and 4.926 billion cycles. Both incurred about 4,097,960 minor faults, close to the 4,096,000 4-KiB pages needed to first-write two complete outputs per submission. These counts support a first-touch workload and an NT regression in that workload; they do not isolate a particular page-fault replay or write-combining mechanism.

Additional measurements alternated streamed/temporal variants in three fresh-process repetitions per dtype, shape and output mode. Fixture construction is outside the loop; submissions allocate, publish and release their results normally. The measured NT/temporal time ratios below are medians; values below one favor NT. All 84 completed records for these 14 mode/shape/dtype combinations are retained in `public-store-policy.jsonl`. A further 4096² fp64 probe reached the fixture's managed-capacity limit and is not included as a performance result.

| Dtype / square side | Bytes per output | Joint NT/temporal | Variance NT/temporal |
|---|---:|---:|---:|
| fp32 / 256 | 0.25 MiB | 1.002 | 1.000 |
| fp32 / 1024 | 4 MiB | 1.533 | 1.465 |
| fp32 / 2048 | 16 MiB | 1.228 | 1.220 |
| fp32 / 4096 | 64 MiB | 0.902 | 0.985 |
| fp64 / 256 | 0.5 MiB | 1.002 | 1.004 |
| fp64 / 1024 | 8 MiB | 1.297 | 1.505 |
| fp64 / 2048 | 32 MiB | 0.964 | 1.006 |

The runtime now retains ordinary stores below 32 MiB per output, using the same production packet arithmetic, and permits deferred NT stores at or above that size. This is a measured policy for the available Intel host; the variance difference at 32 MiB is only 0.6%, not treated as a significant win. The header's standalone reused-buffer thresholds remain 128 KiB for joint and 2 MiB for variance. Both fp32/fp64 public paths have checks immediately below, at and above the runtime threshold, with variance and joint outputs.

The final production runtime repeats the 2,000-call test in **3.204 seconds**, with 4.912 billion user core cycles and 4,097,956 minor faults: task clock falls by **33.3%**, while fault count is essentially unchanged. Final sampling attributes 30.67% to the temporal joint packet body and 5.14% to operation traversal. Another 45.58% is in an unresolved kernel symbol; it is not assigned to a named kernel function. The remaining first-touch, allocation and scheduling cost is not removed by changing the store instruction. The final public-MAD profile still attributes 16.58% to mutex unlock, 13.28% to mutex lock, 16.15% to budget consumption, 11.87% to bracket selection and 10.83% to estimator traversal. Those accounting services belong to the frozen executor.

The frozen binder requires explicitly copied inputs when a runtime affine layout disagrees with the compiled view. Negative-stride/unaligned bindings were verified to receive that existing COPY-required diagnostic; this record does not claim a new direct affine-binding capability.

### Public call timings

These fp32 measurements run in separate timing-only processes, without the correctness suite first. Each process measures 32² MAD/joint followed by 1024² MAD/joint; compilation and preparation are outside submit-to-join timing. First call is reported separately. The warm column is the upper median of the following six calls. Result ownership is retained until join returns and released before the next iteration. This is a small diagnostic sample, not the 182-case numerical gate or a compiler ranking.

| Build | Operator / side | Compile (µs) | Prepare (µs) | First submit/join (µs) | Warm submit/join (µs) |
|---|---|---:|---:|---:|---:|
| M5 Clang O2 | mad / 32 | 183.79 | 12.38 | 136.96 | 50.75 |
| M5 Clang O2 | joint / 32 | 57.92 | 3.42 | 53.62 | 52.04 |
| M5 Clang O2 | mad / 1024 | 37.12 | 2.96 | 2842.08 | 2508.38 |
| M5 Clang O2 | joint / 1024 | 44.46 | 2.42 | 581.04 | 237.00 |
| i9 Clang O3 | mad / 32 | 105.37 | 7.81 | 143.80 | 55.73 |
| i9 Clang O3 | joint / 32 | 81.18 | 5.77 | 96.46 | 71.99 |
| i9 Clang O3 | mad / 1024 | 68.17 | 5.53 | 3816.68 | 3357.82 |
| i9 Clang O3 | joint / 1024 | 83.19 | 5.20 | 1629.59 | 1477.94 |
| i9 icpx O3 | mad / 32 | 100.74 | 8.28 | 141.51 | 66.24 |
| i9 icpx O3 | joint / 32 | 88.82 | 4.92 | 85.42 | 72.83 |
| i9 icpx O3 | mad / 1024 | 68.03 | 6.27 | 3558.41 | 3223.42 |
| i9 icpx O3 | joint / 1024 | 84.35 | 5.32 | 1615.55 | 1461.58 |

The first process-level compile/submit begins with cold process state. Its difference from later cases is recorded separately and is not attributed to a single internal component. M5 preparation and first calls are measured rather than inferred from warm-loop throughput. Linux 1024² joint remains affected by fresh result pages, even after the store-policy improvement.

### Maximum-size public integration

All three runtime builds also execute **16385² fp32 and fp64** through compile/prepare/submit/join and result publication. The fixture is the ramp I(y,x)=y+x, giving exact zero MAD; joint values are checked at boundary/interior positions. These are correctness/resource checks, not another random-input performance comparison. The table records one execution per build/dtype; it is not a timing median.

| Runtime | Dtype | Fixture creation (ms) | MAD submit/join (ms) | Joint submit/join (ms) | Host allocator watermark after joint |
|---|---|---:|---:|---:|---:|
| M5 Clang O2 | fp32 | 897.30 | 812.99 | 161.52 | 3,490,182,024 B |
| M5 Clang O2 | fp64 | 944.32 | 1283.22 | 391.64 | 6,711,802,260 B |
| i9 Clang O3 | fp32 | 859.51 | 1213.36 | 238.83 | 3,490,182,148 B |
| i9 Clang O3 | fp64 | 922.56 | 1465.87 | 441.05 | 6,711,802,384 B |
| i9 icpx O3 | fp32 | 882.31 | 1250.89 | 240.74 | 3,490,182,148 B |
| i9 icpx O3 | fp64 | 974.23 | 1512.63 | 440.33 | 6,711,802,384 B |

These checks explicitly grant 16 GiB managed Host/Shared/Payload capacity, a 16 GiB live-byte/result-growth ceiling, maximum Run work and one active task. Default result-growth/resource limits are too small for this fixture and are not changed in the product. MAD scratch crosses 4 GiB; cumulative issued work is about 6.447 billion after MAD and 8.057 billion after joint. These totals include coordinator and fixture history, not just arithmetic work. The Host watermark is a managed allocator statistic, **not process RSS or physical resident memory**.

The numerical matrix therefore does **not** establish a 1.5× end-to-end advantage for every small public Workflow call. At 32², tens of microseconds of submit/join overhead dominate a much smaller kernel. Altering the frozen executor to remove that overhead is outside the permitted scope. Reusing a compiled/prepared workflow removes repeated compile/preparation cost but does not eliminate per-Run scheduling and result ownership.

## Validation and reproducibility

The same runtime acceptance suite passes on **M5 Clang O2, i9 Clang O3 and i9 icpx O3**. Each final JSONL log contains 69 check records, including its completion marker; internal parameter loops perform many more assertions. This is three executions of the same suite, not 207 different test designs. Runtime checks cover:

- Exact selection against an independent sorted reference on 70 rank/pattern subcases, including constant, ascending, descending, repeated and deterministic random arrays, with sizes through 65,537.
- A rank 2–8, explicit/nonterminal spatial axes, batch dimensions, full/sparse masks, masked poison and tile extents 2/8; B fp32/fp64 rank 1–8, batch and 1×65536.
- Selected variance/sigma/joint outputs, ROI support, independent output owners, preparation lifetime, cumulative Run work across tasks, resource failure and zero numerical-refinement requirements for B.
- Native fp64 exponent range, subnormal preservation, safe midpoint, cancellation checkpoints and pre-cancelled public runs.
- Forced numerical streaming alignment/prefix/tail/guard checks: **5,120 variance cases per dtype** and **81,920 joint cases per dtype**, including matching/mismatching output cache-line phases. Public misaligned-row checks and the final 32 MiB runtime threshold checks exercise integration separately.
- Existing COPY-required diagnostics for incompatible affine bindings, without claiming support the frozen binder does not provide.

The permanent oracle runner passes all three RES01 self-test fixtures. Separately, the production numerical implementation is checked against an independent Fraction/MPFR native-staging model on 16 cases (eight patterns × two dtypes): ordinary, cancellation, large exponent, subnormal, ties, variance overflow, read-noise-square overflow and delta overflow. All MAD result bits and **2,016 B values** match on M5, Intel Clang and icpx. The high-precision ideal result is also recorded separately: ordinary MAD absolute error/input scale is approximately 2.11e-8 for fp32 and 5.65e-17 for fp64. At subnormal scales, native quantization can have a much larger relative error; its absolute error and exact native-bit agreement are reported rather than hidden by a relative-only metric.

The A spec's final Float64 scaling tolerance applies to a given native MAD. A separate conservative input-scale envelope covers the tested allowed native groupings, with exact rank checks independent of that scalar tolerance. This clarification admits all actually tested MATLAB candidates; it does not alter production arithmetic or erase failed correctness cases. The MATLAB raw timings are reused and re-summarized, with method choice preserved.

The source audit checks 34 member specs and 12 family contracts for the native contract, and confirms no RES13/14 or frozen kernel changes. Linux operation integration links cached core/SDK archives whose 174 kernel/header source files match the current checkout; filesystem metadata files are excluded from that statement. The numerical build hash audit verifies all six final binaries against their metadata and the current numerical source files. The final runtime-only store-threshold edit does not change those numerical sources or the six timed binaries. Final runtime metadata records the separately rebuilt integration source hashes.

Temporary numerical/runtime acceptance sources and executables are removed after validation as required by AGENTS.md. The independent oracle and reusable performance driver remain. ClangFormat 21, cpplint on changed C++, the bundled-operation build and `git diff --check` are the final source gates. No frozen kernel file or existing ABI layout is modified.

Raw measurements and profiles are retained under `out/res01-performance/`: full JSONL matrices, build metadata/hashes, `summary/summary.json`, `summary/cases.csv`, M5 `sample` reports, Linux `perf stat/report` output, disassemblies and runtime/oracle result logs. The generated summary checks the complete expected case set and rejects missing/duplicate entries. Reproduction commands are in the driver README.

## Primary sources consulted

- K. C. Kiwiel, [Improved randomized selection](https://arxiv.org/html/cs/0402005v1): two-pivot sampling, rank-aware selection and duplicate handling informed the bracket strategy. This implementation uses its own fixed sample schedule and explicit rank verification.
- NumPy [x86-simd-sort implementation notes](https://github.com/numpy/x86-simd-sort/blob/main/src/README.md): SIMD partitioning/selection and small-range handling informed the implementation approach; no new sorting-library dependency was introduced.
- MathWorks [median](https://www.mathworks.com/help/matlab/ref/double.median.html) and [mad](https://www.mathworks.com/help/stats/mad.html): native-class medians and the median absolute-deviation definition. The high-pass estimator still requires explicit composition.
- OpenCV [Mat::convertTo](https://docs.opencv.org/4.x/d3/d63/classcv_1_1Mat.html): affine conversion supplies the optimized composed B reference.
- Google Benchmark [preventing optimization](https://google.github.io/benchmark/user_guide.html#preventing-optimization): escaping memory and compiler barriers informed the timing harness.
- Intel [streaming-store compiler guidance](https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2023-1/qopt-streaming-stores-qopt-streaming-stores.html) and [Alder Lake P-core events](https://perfmon-events.intel.com/platforms/alderlake/core-events/p-core/): fence obligations and distinct split/alias/store-buffer measurements.
- MathWorks [fma](https://www.mathworks.com/help/fixedpoint/ref/half.fma.html): fused arithmetic background. Availability and timing are checked in the installed MATLAB processes rather than inferred from the online release documentation.
