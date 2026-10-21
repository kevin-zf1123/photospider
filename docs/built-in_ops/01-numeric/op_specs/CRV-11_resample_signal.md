---
spec_schema_version: 1
id: CRV-11
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11: signal resampling and antialias filtering

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Position interpolation and antialias low-pass filtering are separate capabilities.
Named linear/PCHIP resampling templates reuse CRV-01. A separate low-pass
specification handles filtering before downsampling. Interpolation alone makes
no antialias guarantee. No implicit periodic Fourier resampling is selected.
Four interpolation templates cover linear/PCHIP times single/multiple functions.
Dynamic ports are positions[K], values[K] or values[K,C], new_positions[N].
Outputs are samples and positions (the new_positions source). Inherit the
corresponding CRV-01 method's numerical, dtype, domain and dependency contract.
Only naming and position export are new; no second interpolation algorithm is
defined by the template. positions output directly forwards the requested
new_positions bits, retaining dtype and including NaN/Inf/sNaN/negative zero;
it has no source positions/values reads or interpolation validation. Samples demand requires CRV-01's finite query and source preconditions. The low-pass contracts below complete the initial scope. The maintainer requires both equally and unequally spaced
signals in the initial low-pass scope; uniform-sampling-only is insufficient.
For unequal spacing, the signal reconstruction and integration measure must be
explicit, not an implicit equal-weight sum over irregularly spaced samples.

Uniform and nonuniform low-pass are independent operations. Nonuniform low-pass
first interprets positions/values as a piecewise-linear continuous signal and
convolves that reconstruction against a specified continuous low-pass kernel,
integrating by coordinate length. It is not a local equal-weight sample average
or unspecified regression smoother. Kernel and boundary behavior follow below.

The maintainer requires multiple window/kernel choices in the initial low-pass
scope. The initial set is sinc times Hann, Hamming, Blackman and Kaiser windows,
plus truncated Gaussian. Each kernel has independent uniform and nonuniform
operation names/specifications. Shared boundary, shape and execution rules are
specified once; Kaiser beta and Gaussian sigma are explicit kernel-specific
parameters. Kernel identity is not an unspecified low-pass mode.

## Resampling template contract

The four templates map positions to CRV-01 x, values to y and new_positions to query. Export the chosen interpolator's values as samples and the independent new_positions binding as positions. Single-function/multi-function shapes and all dtype/count limits match CRV-01A/B/C/D exactly. The samples node uses the existing CRV-01 interpolation operation. The positions node uses `core.identity` to expose the bound new_positions Result, retaining its descriptor, dtype and raw bits, including NaN/Inf/sNaN/negative zero. Its requested support is exactly the requested new_positions region and it has no curve-source dependency. The bound Result must satisfy the compiled structural contract. Sample requests require CRV-01's numerical preconditions.

Authoring static profile=strict/apple_silicon/x86_64 defaults to strict and chooses
the corresponding primitive key. Other static parameters are the source's dtype
(default Float64) and out_of_domain (default reject, with clamp and the source's
defined extrapolation). No implicit low-pass, sample-rate inference, period or
anti-alias quality claim is added. Construction is lazy and outputs remain dynamic
with their bound source snapshots.

Sample requests inherit CRV-01's fixed task execution: each selected output tile reads all source positions, its new-position rows and all source-value rows for its columns. Source-grid closure determines actual physical tile support. Only selected outputs are evaluated and allocated; fixed arithmetic scratch is private to each invocation. Position-only requests copy the exact requested query tiles independently and allocate separate output owners. The two outputs belong to different IR calls, and joint exports require a successful Run. Empty selection emits no payload tasks. Rebinding reuses immutable preparation; there is no content-cache identity promise. Escaped Results retain their owners beyond context lifetime.

Conceptual fixture positions=[0,1,3], values=[0,2,4], new_positions=[2,0.5,2]
gives linear samples=[3,1,3] and positions=[2,0.5,2]. PCHIP uses its own exact
CRV-01 fixture; multi-function uses the corresponding per-column reference.
Test independent positions containing nonfinite bits without running interpolation,
and use valid finite queries for samples. Check output-specific static support, separate output ownership, source error propagation and all CRV-01 numerical/
partial-request acceptance. Public runtime commands are implementation gates;
these are Proposed templates, not new registered interpolation primitives.

- [Linear single-function template](CRV-11A_resample_linear.md).
- [PCHIP single-function template](CRV-11B_resample_pchip.md).
- [Linear multi-function template](CRV-11C_resample_linear_multi.md).
- [PCHIP multi-function template](CRV-11D_resample_pchip_multi.md).

- [Interpolation family](CRV-01_interpolate.md).
- [Curve category](../curves.md).

## Low-pass specifications

- [Uniform shared contract](CRV-11_uniform_lowpass_contract.md).
- [Nonuniform shared contract](CRV-11_nonuniform_lowpass_contract.md).
- [uniform hann_sinc](CRV-11E1_lowpass_uniform_hann_sinc.md).
- [uniform hamming_sinc](CRV-11E2_lowpass_uniform_hamming_sinc.md).
- [uniform blackman_sinc](CRV-11E3_lowpass_uniform_blackman_sinc.md).
- [uniform kaiser_sinc](CRV-11E4_lowpass_uniform_kaiser_sinc.md).
- [uniform gaussian](CRV-11E5_lowpass_uniform_gaussian.md).
- [nonuniform hann_sinc](CRV-11F1_lowpass_nonuniform_hann_sinc.md).
- [nonuniform hamming_sinc](CRV-11F2_lowpass_nonuniform_hamming_sinc.md).
- [nonuniform blackman_sinc](CRV-11F3_lowpass_nonuniform_blackman_sinc.md).
- [nonuniform kaiser_sinc](CRV-11F4_lowpass_nonuniform_kaiser_sinc.md).
- [nonuniform gaussian](CRV-11F5_lowpass_nonuniform_gaussian.md).

## Maintained implementation and validation

The four public resampling templates are maintained through the public `resampling.hpp` authoring helpers. Each helper appends a CRV-01 samples node and a `core.identity` positions node with collision-free IDs; it leaves existing workflow exports unchanged and returns both node outputs for explicit use or export. Samples and positions are Results throughout compilation, binding and execution. The identity output consists of explicit tile copies of the bound query Result. A positions-only selection reads only the requested query tiles and allocates separate output tiles; it does not read curve data. The four helpers introduce no registered interpolation operation of their own.

Temporary public composition checks inherit CRV-01's independent exact arithmetic oracle. Cover all four templates, mixed dtypes, sparse row/column selection, positions-only raw special bits, source layouts, preparation reuse, cancellation, resource admission and escaped owners. Large logical outputs are admitted according to selected tiles and fixed scratch. Verify generated and explicit graph equivalence separately from numerical correctness; no independent resampling formula is introduced.
