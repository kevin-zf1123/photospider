---
spec_schema_version: 1
id: CRV-11
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
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
it has no source positions/values reads or interpolation validation. Only samples
demand invokes CRV-01's finite query and source validation. The low-pass contracts below complete the initial scope. The maintainer requires both equally and unequally spaced
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

The four templates map positions to CRV-01 x, values to y and new_positions to query. Export the chosen interpolator's values as samples and the independent new_positions binding as positions. Single-function/multi-function shapes and all dtype/count limits match CRV-01A/B/C/D exactly. The samples node uses the existing CRV-01 interpolation operation. The positions node uses `core.identity` to expose the bound new_positions Result, retaining its descriptor, dtype, facets and raw bits, including NaN/Inf/sNaN/negative zero. Its requested support is exactly the requested new_positions region and it has no curve-source dependency. Typed and upstream validation of the bound new_positions Result still applies. Only a samples request invokes CRV-01's finite-query and source validation.

Authoring static profile=strict/apple_silicon/x86_64 defaults to strict and chooses
the corresponding primitive key. Other static parameters are the source's dtype
(default Float64) and out_of_domain (default reject, with clamp and the source's
defined extrapolation). No implicit low-pass, sample-rate inference, period or
anti-alias quality claim is added. Construction is lazy and outputs remain dynamic
with their bound source snapshots.

Sample requests inherit CRV-01's Whole complete-input validation, full output allocation/invalidation and Run numerical failures. The operation reads authorized Result windows without collecting or copying its complete tensor inputs. Mathematical stencils remain unchanged; undelivered invalid queries or columns can fail the run. A sparse sample request still publishes full certified coverage, while its recorded dependency support and dirty mapping cover the complete interpolation inputs.
Position-only requests have only the exact requested new_positions source
support and no source curve reads. Joint execution is an optimization, not a
dependency change. Empty requests, arbitrary source strides, owners after context
teardown, cache-off, budgets and cancellation follow the source/forwarding contract.
Account bindings, source read windows, output/intermediate owners and compiled template state rather than treating export aliases as unowned pointers. Cached sample Results associate the current positions, values and query Result identities; the forwarded positions Result associates the current query source. Static interpolation preparation can survive binding replacement while dynamic sample Results are recomputed. Escaped Results and authorized read windows retain their owners after context retirement and handle release.

Conceptual fixture positions=[0,1,3], values=[0,2,4], new_positions=[2,0.5,2]
gives linear samples=[3,1,3] and positions=[2,0.5,2]. PCHIP uses its own exact
CRV-01 fixture; multi-function uses the corresponding per-column reference.
Test independent positions containing nonfinite bits without running interpolation,
then samples demand correctly rejecting those queries. Check output-specific
dirty support, alias ownership, source error propagation and all CRV-01 numerical/
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

The four public resampling templates are maintained through the public `resampling.hpp` authoring helpers. Each helper appends a CRV-01 samples node and a `core.identity` positions node with collision-free IDs; it leaves existing workflow exports unchanged and returns both node outputs for explicit use or export. Samples and positions are Results throughout compilation, binding and execution. The identity output is a mapped view of the bound query Result, so a positions-only demand does not read curve data or allocate a copied position payload. The four helpers introduce no registered interpolation operation of their own.

The maintained resampling executable passes eight manual groups under each of Strict and Apple. It retains the existing CRV-01 exact-copy and numeric-accuracy checks and does not define an independent resampling oracle. The tests cover scalar and multi-column linear/PCHIP results, independent raw and typed position forwarding, position-only dependency support, complete Whole sample support and dirty mapping, two fresh-content cache hits with current direct-source associations, static preparation reuse after binding replacement, upstream failure order, arbitrary source layouts, worker/caller floating-environment preservation and escaped owners. Float64 layout checks cover all four templates and eight source layouts under each caller/worker rounding mode; Float32 checks separately cover bit-preserving positions, typed validation and owner lifetime. K=65536 executes a real endpoint interpolation. At N=2^40 the positions output successfully exposes a zero-copy mapped view, while a samples request is rejected because the complete Whole output exceeds the Root Payload budget. The latter is a capacity rejection, not a numerical run at that output size. The installed 0.32.0 consumer passes its Strict CTest and eight direct Apple groups through the public package. See the [signal-resampling workflow](../../../../examples/numeric_workflow/README.md#signal-resampling).
