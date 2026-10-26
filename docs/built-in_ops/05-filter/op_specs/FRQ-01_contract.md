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
implementation in `frequency_transform.cpp`. They have no strict/accelerated
profile selector. The SDK `photospider/ops/frequency.hpp` exposes
`transform(document, member, inputs, grid)`. No GPU backend is provided.

Direct nodes supply `schema_revision` Int64 1, `original_shape` as a canonical
comma-separated String, `y_axis` and `x_axis` Int64, and `packing`, `index_order`,
`norm`, `unit`, `grid_id` Strings. `spacing_y/x` and `origin_y/x` are Float64.
The original shape includes batch axes. Transform input is unshifted, with full
packing for complex transforms and r2c_x packing for real transforms.

Power-of-two axes use radix-2/radix-4 butterflies. Other axes use radix-2/3/5
factorization with residual codelets through length 31; larger residual factors
use Bluestein power-of-two convolution. Twiddles are ordinary libm sine/cosine
values with exact quarter-turn constants. There is no Ball/DD enclosure, final
rounding-cell check, exact refinement or data-dependent certified replay.

The working plane contains H*W binary64 complex values (16 bytes each), even for
real transforms. Scratch also holds max(H,W) complex line values, up to eight
contiguous columns when h>1, max(h,w)>=1024 and the vertical axis does not use
Bluestein, and max(H,W)+2*B complex values when Bluestein convolution length B is
needed. Every buffer is included in the fixed preparation signature.

Prepared kernels own immutable dimension/direction root tables through managed
Context allocators. Construction occurs outside the publication mutex and only
complete entries are published. Concurrent misses may perform separately charged
constructions; losing copies are released. Cache entries contain no payloads or
Run ledgers. Cache hits skip table generation, not transform work.

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

## Shared Mathematics, Units, and Profiles

Each combination of untransformed axes forms an independent transform group. Spacing and origin annotate coordinates but do not change the DFT phase formula; physical-origin phase compensation requires an explicit multiplication. Under `backward` normalization, the forward scale is 1 and inverse scale is 1/(HW); under `ortho`, both scales are 1/sqrt(HW). Full-complex real and imaginary components have identical shapes. Coefficient outputs use FrequencyGrid/v1.


The four transforms use the **F** ordinary floating-point contract. Both dtypes
use binary64 working arithmetic; output converts to the input dtype. Ordinary
rounding occurs at machine operations, twiddle generation and normalization.
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

This is the usual approximate-FFT model, not a promise to duplicate OpenCV or
MATLAB internals. In particular, this implementation's binary64 workspace differs
from MATLAB's native single-precision computation for single input.

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
