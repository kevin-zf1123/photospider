---
spec_schema_version: 1
id: FRQ-01
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-01A
- FRQ-01B
research_sources:
- S09
- S10
- S25
---

# FRQ-01: Two-dimensional complex DFT

The mathematical DFT is separate from its FFT implementation. The implementation returns an ordinary floating-point approximation, without a correctly rounded final-output requirement.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-01A | [Two-dimensional complex forward transform](FRQ-01A_fft2.md) | primitive | F |
| FRQ-01B | [Two-dimensional complex inverse transform](FRQ-01B_ifft2.md) | primitive | F |

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Runtime integration and remaining acceptance

`frequency.fft2` and `frequency.ifft2` are portable CPU operations registered by
`register_frequency`; `frequency.rfft2` and `frequency.irfft2` share their
implementation in `frequency_transform.cpp`, the planned execution headers
`frequency_fft_execution.hpp` and `frequency_fft_planned.hpp`, and the scalar
fallback in `frequency_fft.hpp`. They have no strict/accelerated
profile selector. The SDK `photospider/ops/frequency.hpp` exposes
`transform(document, member, inputs, grid)`. No GPU backend is provided.

Direct nodes supply `schema_revision` Int64 1, `original_shape` as a canonical
comma-separated String, `y_axis` and `x_axis` Int64, and `packing`, `index_order`,
`norm`, `unit`, `grid_id` Strings. `spacing_y/x` and `origin_y/x` are Float64.
The original shape includes batch axes. Transform input is unshifted, with full
packing for complex transforms and r2c_x packing for real transforms.

On GNU/Clang x86-64, the planned packet path requires AVX and FMA; AArch64 uses NEON. Explicit fused multiply-add instructions are part of the ordinary F arithmetic contract. Compilation retains strict floating-point flags. Other targets and x86 CPUs without the required instructions use the scalar engine.

Float32 power-of-two rows of length 16 through 128 use fixed natural-order codelets. Matching column axes of length 16 through 65536 use native four-lane butterflies in compact in-place DIF recursion when the working pitch is divisible by four. Each task owns eight columns on x86; Apple AArch64 uses sixteen columns from height 256 to cover its 128-byte cache lines, and eight for smaller axes. A smaller final band, still a multiple of four columns, uses the same bounded codelet. Intel Float32 rows from 16384 through 65536 use a balanced two-factor decomposition with fixed leaves. Other power-of-two axes of length at least 256 retain prepared radix-2/radix-4 plans. AArch64 64/128-point Float32 rows keep their internal 16/32-point leaves in separate real/imaginary vector packets; external complex storage remains interleaved.

Float64 dispatch selects `ShortDoubleTransform` with native two-complex packets for power-of-two row lengths 8 through 128 and for 1024-point rows. Row leaves of length 8/16 retain their arithmetic in registers; larger selected rows recursively decimate into four leaves. Native columns use bounded DIF recursion and direct natural-order stores for power-of-two lengths 8 through 128, 1024/2048, and 4096 through 32768 on both platforms, plus 256/512 on x86. Apple AArch64 rows of power-of-two length 16384 through 65536 select the balanced two-factor `SingleTransform`; x86 selects it for these batched rows when the pool cap is one. Batched rows use its serial per-row entry within the outer row phase. Other power-of-two axes of length at least 256 retain the prepared power plan unless selected by the single-line path below. A single continuous column follows the row policy outside that single-line case. An odd working pitch uses paired native columns and one scalar mixed-plan tail. The 128-point row workspace is 160 complex values; columns use at most eight times the height, within the common worker reservation. The existing managed binary64 root table supplies every level's coefficients.

When a working plane has just one FFT line, power-of-two axes from 16384 through 65536 can use a two-factor parallel path if the FRQ pool has more than one participant. Each factor is 128 or 256. First-stage tasks transform disjoint input columns into compact natural-order private bands, then transpose and multiply into the shared N-value intermediate. Coefficients are prepared in that intermediate's row-major order, so the transpose stores use continuous coefficient packets. After joining, the second phase transforms disjoint intermediate column bands directly into the existing output working plane, with no whole-axis copy. Float32 uses column-mode fixed plans; Float64 uses the 128/256-point root tables matching its factors. The immutable coefficient matrix is keyed by (N, inverse) in the precision-specific prepared cache, uses the Context allocator and replaces the former whole-axis root table on this path. It is published only after complete construction; a cancelled call may still leave a complete reusable table. A pool cap of one uses the serial entry of the same two-factor implementation.

The intermediate plus all participants' private bands fit within 2N complex values. Let band be eight columns on x86 or sixteen on Apple AArch64. The first-stage band is half that width, leaving separate equal-sized private regions for native scratch and compact output within each worker's existing band workspace; the second-stage band uses the full width. The participant count is at most A/band with A<=B, so all private regions occupy at most N values. The dimension-derived reservation still covers the existing serial and scalar fallbacks. Participant selection additionally respects the available bands and scratch. The single-line path selects its participants once from the combined declared cost of both phases; on Apple AArch64, both phases use at most four participants regardless of that cost. The pool cap, available bands and scratch may reduce this count further. With native column cost C, the phase costs are A*C(B)+64N and B*C(A) for N=A*B. Building a missing coefficient matrix admits 128N units, and arithmetic/transpose segments remain bounded by 1024 units. No additional per-Run allocation is introduced.

Other packet transforms use mixed-radix plans with fixed radix-2/3/4/5 butterflies, bounded odd codelets through radix 31, and a fused initial eight-point stage. Factor ordering exposes the binary stages before odd stages when possible. Radix-5 packets use a bounded unrolled schedule; their cancellation checkpoint precedes live vector temporaries. On x86, radix-7/11/13 stages also use explicit expansion with cancellation checkpoints before each complete output pair, keeping inner accumulators away from callbacks without changing the declared work. AArch64 retains the smaller schedule for these odd radices. Bounded factor discovery also recognizes prime factors through 257. Proper factors larger than 31 become individual convolution stages; the whole composite axis is not padded solely because it contains such a factor. A residual uses Rader convolution when its predecessor has only factors through 31 and the bounded primitive-root search certifies the required order; otherwise it uses Bluestein convolution. The forward DIF and inverse DIT convolution pair shares a permuted spectrum, avoiding an intermediate permutation.

Smooth mixed-radix axis plans use Good–Thomas permutations at coprime stage boundaries, omitting those boundaries' twiddle multiplications. These permutations and their inverse frequency map belong to the immutable prepared plan; recursive convolution plans retain their own layout. A final ordinary radix-3/5 column stage can fuse its butterfly with natural-order output materialization for packet widths up to four, avoiding a separate scratch write and scatter pass. Both Float32 and Float64 use this path when the geometric and plan conditions hold. On x86, Float64 mixed column batches of length at least 256 can use four interleaved complex lanes when the complete plan and its leaf convolution use only radices 2/3/4/5. This requires a pitch divisible by four or a leaf convolution; remaining columns keep the two-lane/scalar paths. Row batches retain their separate selection. This property is independent of the existing wide-plan flag used by Float32.

Adjacent independent transforms share SIMD packets. Columns outside the fixed codelets choose direct packets, fused bit-reversal gather, or blocked transpose buffers with pitch H+1. Transpose bands hold at most 64 columns; x86 heights from 4096 use at most eight. Tall, thin planes may use narrower bands to expose enough tasks. Dispatch depends on dimensions, dtype and available CPU instructions. Twiddles use ordinary binary64 sine/cosine with exact quarter-turn constants, converted to the working dtype before data arithmetic.

Complex working values occupy S=8 bytes for Float32 and S=16 bytes for Float64. Let P be the working plane width and R the row FFT length: P=W/2+1 and R=W/2 for even-width real transforms; otherwise P=R=W. The fixed scratch allocation is

\[
S\left(HP+\max(F,KD)\right),
\]

where F is the scalar fallback's line, column and convolution workspace. The helpers in `frequency_fft_layout.hpp` define K and D independently of runtime thread selection:

- K is 1 when HP<4096. For 4096<=HP<16384 it is `min(8, max(R>1 ? H : 1, H>1 ? P : 1))`; otherwise the first limit is 32. This dimension-derived reservation applies even when a runtime cap selects one participant.
- The line workspace is `8 * (max(H,R) + max(C(H),C(R)))` complex values. C bounds the largest residual convolution or gathered large-radix stage plus its convolution.
- Auxiliary column workspace is zero for H<256, P<=1, selected native Float64 columns, and non-power-of-two H<8192. These paths fit within the line workspace.
- Remaining transpose paths reserve `min(P,B)*(H+1)`, where B is 8 on x86 at H>=4096 and 64 otherwise. Aligned power-of-two columns with P divisible by four need no separate transpose area below H=4096 on x86 or H=8192 elsewhere.
- After these rules, Apple AArch64 aligned power-of-two axes from 256 through 65536 that did not already take the native Float64 zero-workspace case retain at least 8H auxiliary values. Together with the line workspace, this covers the native Float32 sixteen-column band, including thin final bands.
- For a single continuous FFT line (`H==1` or `P==R==1`) whose actual complex axis length N is 16384, 32768 or 65536, K is one and D is 2N. For horizontal compact real transforms N=R, and the extra Nyquist value remains in the HP working plane. Other shapes use the sum of line and transpose workspaces rounded up to eight complex values.

Each participant receives a separate D-sized slice. The scalar fallback and planned path reuse the same admitted allocation. Scratch depends on dtype, dimensions and the compilation target's scalar fallback strategy; it is included in the preparation signature. Regenerate compiled Plans when changing this implementation or target.

Input traversal resolves a complete contiguous plane once when its views permit it, using separate
affine row strides for the real and imaginary inputs. Other layouts retain the tile/row cursor. SIMD
packing and output conversion use alignment-safe accesses. A complex transform with two matching
output views can write real and imaginary components in one pass; each view keeps its own
allocation, byte offset and row stride. Other output selections retain individual materialization.
Paired output traversal declares 32 scalar work units per complex sample and 16 per row; admission,
local shares and cancellation checkpoints use this same bound. A continuous single-column working
plane packs its samples as one contiguous segment when each input row stride is one scalar. Matching
outputs use contiguous component conversion; paired complex output polls its row overhead in chunks
of at most 32 samples. The declared work remains unchanged. Contiguous single-column RFFT output
gathers canonical conjugate representatives into native packets and preserves structural positive
imaginary zeros, retaining 64 units per output component per sample. Other RFFT boundary layouts and
compact IRFFT unpacking retain their separate paths. On x86, a complete contiguous single row or
scalar-stride single column with at least 32768 packed samples can divide input packing into
2048-sample chunks. The memory phase selects at most min(pool size, 8, floor(samples/8192))
participants. Chunks are claimed dynamically, so this selection rule does not guarantee a particular
sample count for each worker. Every chunk has an independent exact Work share. Compact real input
counts pairs of real samples and constructs the final Nyquist zero only after the chunks join.
Compact inverse input includes its supplied Nyquist element. Other layouts retain their original
traversal. Signed strides, selected axes, batch coordinates and independently allocated tiles remain
supported. Selecting output tiles retains the specified whole-plane input dependency.

Prepared kernels own separate Float32/Float64 caches of immutable root tables, power plans, mixed-radix plans and fixed codelet plans through managed Context allocators. Fixed plan keys distinguish row and column decompositions. Plans hold their permutations, stage twiddles, bounded codelet coefficients and any Rader orbit or Bluestein chirps and transformed convolution spectrum. Construction occurs on the invocation's calling thread outside the publication mutex, and only complete entries are published. Concurrent misses may perform separately charged constructions; losing copies are released. Entries retain dimension-derived data, with no payload or Run ledger. Cache hits skip plan generation, not transform work.

The process-local FRQ pool defaults to the reported hardware concurrency, capped at 32 participants including the caller. On Linux, this default is also capped by the calling thread's allowed CPU count at pool construction. `PHOTOSPIDER_FRQ_THREADS=N` supplies an explicit positive cap, clamped to 32 and read only when the pool is first constructed. Outside the single-line path described above, the ordinary axis selector keeps phases with fewer than 2^20 declared work units serial. At or above that threshold, it allows one participant per 2^19 units, subject to the pool, task and scratch capacities. On Apple AArch64, those ordinary axis phases use at most four participants when their declared cost is below 2^24 units. Packing, real recombination and output retain their separate thresholds. Small serial phases do not construct the pool, and insufficient independent tasks further reduce participation. On x86, packing, recombination and output phases with at least 2^28 declared scalar work units additionally cap participation at eight to limit memory contention; independent FFT phases retain their own participant selection. The host's cancellation query borrows the Run's retained token and performs no shared-owner copy per poll. This operator setting is separate from the host's Run lane limit and does not bind threads to cores.

On x86 each pool worker has its own condition variable, and a phase notifies only its selected participants. Shutdown notifies every worker. AArch64 keeps a shared condition: full-pool jobs broadcast and retain the physical worker-to-slot mapping; partial jobs notify only the requested number of waiters while holding the publication mutex. These workers claim unique logical scratch slots for that job. Job completion, failure draining and shutdown retain the same join boundary.

The pool owns worker threads and scheduling state. An invocation borrows the pool until all participants finish; a concurrent invocation uses its caller if the pool is busy. The pool cap is not a global cap on all concurrent callers in the process. Input packing, independent FFT lines, real-spectrum recombination and output rows can run in separate tasks. Plans and input views are read-only; participants use disjoint output regions and scratch slices. Each phase admits its complete deterministic cost once through the caller's Work service. Participants receive independent local counters with finite shares and retain the original cancellation callback. After joining, the caller checks the host's sticky failure on both normal and exception paths. Every participant restores its floating environment before releasing the job. No Run data remains in the pool after return.

The mixed-plan double radix-four bound is 160 units per independent complex lane for scalar and native/wide packet paths. The fused eight-point discount is derived from the current radix-two/four costs. Adjacent double axis phases combine redundant completion probes; every payload segment still retains its pre-poll, and the axis retains a final completion probe.

For the short double codelets, row cost is 40N for N<=16 and 72N+4R(N/4) otherwise; column cost is 32N for N<=8 and 48N+4C(N/4) otherwise. Row gather/combine segments declare 256/320 units, and a two-column radix-four segment declares 384. The largest indivisible leaf segment declares 640 units. These bounds include construction, scalar/control work and both AVX/NEON layouts. A paired-column phase and its optional scalar tail receive exact disjoint shares of their combined declared cost. Native Float64 column participation is selected from the paired work alone; paired work of at most 2^20 units remains serial. The scalar tail does not force a marginal parallel phase.

FRQ-01..06 use typed CPU invocation ABI9. One Run ledger covers all its nodes,
tasks and activations under the Context root. Node `max_work` limits each
invocation independently. Deterministic blocks admit their conservative declared
cost before payload computation; cancellation checkpoints inside and between
blocks are at most 1024 scalar work units apart. Admission requests may exceed
1024. The first failure stays terminal and admitted work is not refunded.
Transforms request zero refinement stages, including cold table construction;
`psx --max-stages 0` therefore permits them. Other FRQ members may refine.

`psx --max-work N` sets the Run limit; the default is 268435456. Work accounting
measures declared scalar/control costs, not CPU instruction counts. Resource
refusal or cancellation prevents successful final Result publication and retains
its existing status/attribution. Root storage, scratch and outputs remain subject
to managed capacity limits. Resource exhaustion does not change precision or
select an unmetered fallback.

Executor-created Results use an aggregate growth guard of `max(1 GiB, ExecutionContextConfig::maximum_live_bytes)`, including retained backing capacity and publication metadata. The default remains 1 GiB. Raising the context Payload allowance above this guard also permits larger individual Results; Host, Payload and Metadata resource limits still apply independently. Complete 16384×16384 Float32 outputs can therefore be published when the context admits both components, scratch and metadata. A smaller explicit output selection retains the complete transform dependency.

## Shared Mathematics, Units, and Profiles

Each combination of untransformed axes forms an independent transform group. Spacing and origin annotate coordinates but do not change the DFT phase formula; physical-origin phase compensation requires an explicit multiplication. Under `backward` normalization, the forward scale is 1 and inverse scale is 1/(HW); under `ortho`, both scales are 1/sqrt(HW). Full-complex real and imaginary components have identical shapes. Coefficient outputs use FrequencyGrid/v1.


The four transforms use the **F** ordinary floating-point contract. Float32
inputs use native binary32 working values, butterflies, accumulation, real
recombination and output scaling; Float64 uses binary64. Dimension-derived
angles, trigonometric coefficients and normalization constants are computed
in binary64 and converted to the working dtype before data arithmetic.
Ordinary rounding occurs at machine operations, coefficient conversion and
normalization.
Correctly rounded mathematical outputs, per-component ULP bounds and cross-library,
cross-algorithm or cross-platform bit identity are not required. Near-zero
coefficients may contain roundoff residuals. Tests compare numerical errors against
an independent reference as defined by the [acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance).

Gradual underflow follows the CPU floating environment. Intermediate overflow and
invalid arithmetic may produce Inf or NaN even if an exact expression would be
finite. NaN payload/priority, exceptional-value propagation and signed-zero bits
follow the executed FFT, without NUM exact-expression ordering or recovery.
No finite-only validation scan is introduced. Explicit RFFT structural imaginary
zeros and boundary conjugate construction remain specified in FRQ-02.

Float32 has the intermediate range and rounding of native single-precision
arithmetic. It can overflow or underflow sooner than an implementation that
promotes Float32 data to binary64; no value-dependent precision retry occurs.
The acceptance tolerances are unchanged. Native dtype computation follows the
usual approximate-FFT model without promising OpenCV/MATLAB algorithm or bit
identity.

## FrequencyGrid/v1 logical schema

The proposed FrequencyGrid/v1 SDK/workflow representation pairs real and imaginary coefficient Results with explicit static transform configuration: original spatial shape, transformed axes, sampling spacing, coordinate origin/unit, normalization, index order and packing. Shapes and the directory are available before coefficient computation. Real and imaginary tensors have compatible schemas, while each materialized tile owns a distinct allocation. A selected transform coefficient plane has its specified whole-plane mathematical dependency unless locality is explicitly guaranteed. Edited coefficients may come from any conforming producer; numerical and payload association premises are caller obligations. Results carry no implicit frequency-grid metadata or numerical validity certificate.

## Shared Execution and Numeric Requirements

F denotes the ordinary floating-point transform rules above. The E/B/S rules and NUM strict/accelerated bounds continue to apply to other members according to their individual specifications. Shape, fixed-read, resource and publication contracts are unchanged by the numeric category.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a complete-input dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
