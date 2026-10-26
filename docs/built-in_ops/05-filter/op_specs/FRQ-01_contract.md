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

The mathematical DFT is separate from its FFT implementation. The strict result is not defined by the operation order of any particular FFT library.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-01A | [Two-dimensional complex forward transform](FRQ-01A_fft2.md) | primitive | E |
| FRQ-01B | [Two-dimensional complex inverse transform](FRQ-01B_ifft2.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Runtime integration and remaining acceptance

The two members are registered by `register_frequency` in
`plugins/ops/05-filter/frequency_operations.cpp`, reached from the maintained
built-in registry. `frequency_transform.cpp` implements certified radix-2/radix-4
FFT, common-twiddle radix-2/3/5 codelets, paired residual odd-radix combinations through radix 31,
Bluestein for large residual factors, and bounded block recurrences for enclosed
roots/chirps. `frequency_direct.hpp` streams directed
refinement and exact trace-square zero/midpoint decisions, including algebraically
correlated ortho normalization. Scratch has a full-plane and linear-axis bound;
it allocates no polynomial arrays proportional to the primitive root order. The SDK helper is declared in
`photospider/ops/frequency.hpp`. CPU strict and target-gated Apple Silicon and
x86-64 keys exist; only the local Apple CPU has runtime evidence in this task.
There is no GPU backend.

Direct nodes supply the FrequencyGrid/v1 configuration explicitly:
`schema_revision` Int64 1, `original_shape` canonical comma-separated String,
`y_axis`, `x_axis` Int64, `packing`, `index_order`, `norm`, `unit`, `grid_id`
Strings, and `spacing_y/x`, `origin_y/x` Float64. Origins/spacings use y,x order.
`original_shape` includes batch prefixes. Forward inputs have original shape;
the inverse checks its stored coefficient shape against this configuration.
A different normalization override is not introduced by the helper.

FRQ-01..06 execute exclusively through typed CPU invocation/preparation ABI 9.
The borrowed work/stage service charges arithmetic and integer-limb segments
before execution, and charges each explicit refinement attempt. One Run ledger
covers all nodes, tiles and activations; repeated/concurrent Runs have independent
limits under their shared Context root. A host budget refusal is `ResourceExhausted`
with `WorkLimit` or `StageLimit`, `Resource`/`Run` attribution and known node/task;
accepted work drains and no final Result is published. The operator's
invocation-local work consumer locks its first failure, including a node-local
limit refusal, service refusal or cancellation; subsequent helper calls preserve
that reason and issue no further admission. Run cancellation retains terminal
priority and does not refund admitted work. The optional Int64 node `max_work`
continues to limit each invocation independently; it is not the Run total.
`ExecutionOptions::maximum_work` defaults to 268435456; callers must explicitly
budget large transforms. `maximum_refinement_stages` defaults to UINT64_MAX and
allows zero. `psx --max-work N --max-stages N` configures these Run limits.
`ExecutionDiagnostics::issued_work` reports successful admission units, excluding
legacy ABI8 internal arithmetic, allocator overhead and CPU instructions.

The H08 cumulative-budget implementation gap is closed. Invocation-8 FRQ Plans
must be recompiled; there is no silent ABI downgrade. Module ABI8, native service
ABI2, traits30 and psir/psxp5 layouts remain unchanged, and other ABI8 operations
remain supported. CPU x86 execution, GPU support and unmeasured platforms are not
implied by the local Apple evidence. Exact fallback is finite-budget computation;
its constant-storage trace test can require quadratic work in input terms and
does not promise FFT-time completion for adversarial unresolved outputs.

Power-of-two lines use binary bit reversal followed by an initial radix-2 stage when needed and radix-4 stages. Each radix-4 group contributes three 512-unit Complex products and a 512-unit add/rotate segment to its line's conservative work bound. Mixed-radix codelets apply one common twiddle per child before combining radix-2, radix-3 or radix-5 outputs. Residual odd codelets through radix 31 pair child indices j and r-j: their sum multiplies a certified cosine, and their difference multiplies a certified sine with an exact quarter-turn rotation. The two output frequencies use opposite signs for the rotated term. The declared combination cost is 512*r + 192*p + 896*p*p, where p=(r-1)/2; recursive visits and output copies are charged separately. Irrational coefficients retain certified Ball enclosures. Work units cover internal certified scalar arithmetic and integer-limb work; CPU instruction counts are a separate measurement.

A deterministic FFT or mixed-radix line admits the exact sum of its declared segment costs before payload computation. This admission includes the line's permutation, recursive child visits, combination and final copies. Deterministic table copies, permutation construction and column gather/scatter use the same pre-admission rule; a partial final column block charges its actual column count. The payload checks cancellation independently, at intervals of at most 1024 scalar work units. An admission request may therefore exceed 1024 units while retaining the shared cancellation interval. Dynamic root certification and directed/exact refinement retain their individual work/stage admission. A refused block performs no payload work, and the first failure remains terminal. Cancellation after a successful admission retains the admitted charge, following the ABI9 rule that work is not refunded.

Prepared kernels own immutable certified root tables keyed by length, requested count and direction, and immutable binary-permutation tables keyed by length. Their managed allocators capture the preparing Context resource root; table objects, map nodes and vector capacities count toward Host and Metadata while their owners remain alive. Table construction occurs privately outside the publication mutex. Only a complete, successfully checked entry is published; concurrent misses may perform separate charged constructions, and a losing private copy is released. A later Run failure does not roll back earlier complete entries. Cache hits pay for the actual copies/visits, while root refinement is charged only when performed. Warm cache use does not waive normalization, exceptional-value or final-certificate work.

For h>1, max(h,w)>=1024 and a non-Bluestein vertical axis, the transform gathers up to eight columns into contiguous certified Complex buffers, transforms them sequentially and scatters them back. The declared scratch explicitly adds h*min(8, transform_width) Complex values, each containing two full Balls; it does not depend on unused space in another numerical layout. The last block uses the remaining columns. Compact real transforms use their compact width for this allocation and retain separate reconstruction roots. Scratch is part of the fixed preparation signature, so Plans compiled with the previous scratch contract must be recompiled.

Full-complex power-of-two transforms can use a 32-byte complex double-double
center with one certified error bound for the plane. Each component stores a
normalized binary64 hi/lo pair. The candidate requires nonzero input magnitudes
in [2^-200, 2^200], certified complex root errors at most 2^-80 and finite
intermediate amplitude bounds; periodic or sufficiently sparse inputs use the
Ball path. The amplitude bound A encloses the Euclidean norm of every exact
complex intermediate, and E encloses the Euclidean error of its stored center.
Outward-rounded stage recurrences include root uncertainty, omitted low-low
products and gradual underflow. ARM64 two-lane arithmetic preserves the scalar
operation order and explicit FMA points. The full Ball scratch capacity remains
reserved for fallback.

The DD candidate checks final normalized rounding enclosures. A Float64 RN-cell
shortcut accepts only an enclosure strictly inside the hi component's rounding
cell; other cases use the general profile gate. Quarter-frequency outputs whose
roots divide four share sixteen certified input sums indexed by (y mod 4,
x mod 4). One input scan constructs these sums, and signed component reductions
recover each requested quarter-frequency coefficient without intermediate output
rounding. This work is admitted and cancellation-polled like other deterministic
blocks. An unresolved DD plane reloads the original inputs and replays the Ball
FFT; the failed candidate, reload and replay all remain charged. Unresolved final
Ball outputs retain directed/exact refinement. The choice of internal candidate
does not introduce a public RN stage or change per-invocation max_work semantics.

The FFT candidate accepts only finite-input expressions whose final enclosure meets the selected profile. Nonfinite inputs and unresolved zero, midpoint or range cases use the original Reader-based directed/exact path, retaining structural-zero exclusions and written-order exceptional-value semantics. Real-input compact storage is described in [FRQ-02](FRQ-02_contract.md#runtime-integration-and-remaining-acceptance).

## Shared Mathematics, Units, and Profiles

Each combination of untransformed axes forms an independent transform group. Spacing and origin annotate coordinates but do not change the DFT phase formula; physical-origin phase compensation requires an explicit multiplication. Under `backward` normalization, the forward scale is 1 and inverse scale is 1/(HW); under `ortho`, both scales are 1/sqrt(HW). Full-complex real and imaginary components have identical shapes. Coefficient outputs use FrequencyGrid/v1.


## FrequencyGrid/v1 logical schema

The proposed FrequencyGrid/v1 SDK/workflow representation pairs real and imaginary coefficient Results with explicit static transform configuration: original spatial shape, transformed axes, sampling spacing, coordinate origin/unit, normalization, index order and packing. Shapes and the directory are available before coefficient computation. Real and imaginary tensors have compatible schemas, while each materialized tile owns a distinct allocation. A selected transform coefficient plane has its specified whole-plane mathematical dependency unless locality is explicitly guaranteed. Edited coefficients may come from any conforming producer; numerical and payload association premises are caller obligations. Results carry no implicit frequency-grid metadata or numerical validity certificate.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a complete-input dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
