---
spec_schema_version: 1
id: FIL-14A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-14
function: gabor_kernel
proposed_operation_keys:
- filter.gabor_kernel_strict
- filter.gabor_kernel_accelerated_apple_silicon
- filter.gabor_kernel_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S01
- S26
---

# FIL-14A: gabor_kernel

Generate complex Gabor kernel. Status **Proposed / D1_draft**.

Inherits the [FIL-14 family contract](FIL-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

No dynamic input → `kernel_real,kernel_imag` Float64 arrays of shape `[2ry+1,2rx+1]`.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static parameters and valid domains

frequency f∈[0,.5] cycles/pixel; theta and phase are finite radians; sigma>0; aspect γ>0; radius_y/x≥0; `dc=keep|remove`; `normalization=none|l1_envelope`.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Parameter domains, structure, integer overflow, resources, cancellation, and upstream errors remain governed by their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **B**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

X=x*cosθ+y*sinθ, Y=-x*sinθ+y*cosθ; E=exp(-(X²+γ²Y²)/(2σ²)); z=E*(cos(2πfX+phase)+i sin(...)). For keep, round each complete component expression once to RN64. For remove, subtract the exact continuous-valued discrete full-kernel mean, then round to RN64 (the stored sum is therefore not necessarily zero). For l1_envelope, divide by exact ΣE, then round to RN64. Apply DC removal before normalization.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data / Control / Validation / Descriptor

For each kernel output, remove/normalization may depend on all static taps; there is no source Data dependency.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, resources, and execution

Generate in high precision in O(A), with a certified enclosure for the full-kernel sum; do not construct external goldens from rounded trigonometric values.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

At f=0, phase=0, keep yields a real Gaussian envelope and zero imaginary part. The theta+π/phase relation yields conjugate symmetry. After remove, stored residual DC is not guaranteed to be zero.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil14.transcend.gabor_kernel(*, frequency, theta=0, phase=0, sigma=1, aspect=1, radius_y=1, radius_x=1, dc='keep', normalization='none')` → [source](../../../../oracle/ops/filter/fil14/transcend.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, ROI/dirty behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU-accelerated variant is registered or implemented. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Computation scope and multistage buffers follow this member and its family contract; use a Run-shared stage only for the global work that contract requires.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
