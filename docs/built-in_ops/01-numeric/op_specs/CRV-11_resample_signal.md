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

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Position interpolation and antialias low-pass filtering are separate capabilities. Named linear/PCHIP resampling templates reuse CRV-01. A separate low-pass specification handles filtering before downsampling. Interpolation alone makes no antialias guarantee. No implicit periodic Fourier resampling is selected.

Low-pass filtering covers both equally and unequally spaced signals. For unequal spacing, the signal reconstruction and integration measure are explicit, not an implicit equal-weight sum over irregularly spaced samples. Uniform and nonuniform low-pass are independent operations. Nonuniform low-pass first interprets positions/values as a piecewise-linear continuous signal and convolves that reconstruction against a specified continuous low-pass kernel, integrating by coordinate length. It is not a local equal-weight sample average or unspecified regression smoother.

The kernel set is sinc times Hann, Hamming, Blackman and Kaiser windows, plus truncated Gaussian. Each kernel has independent uniform and nonuniform operation names/specifications. Shared boundary, shape and execution rules are specified once in the shared contracts; Kaiser beta and Gaussian sigma are explicit kernel-specific parameters. Kernel identity is not an unspecified low-pass mode.

## Resampling template interface

Four interpolation templates cover linear/PCHIP times single/multiple functions. Dynamic ports are positions[K], values[K] or values[K,C], new_positions[N]. Outputs are samples and positions (the new_positions source). The templates map positions to CRV-01 x, values to y and new_positions to query. Single-function/multi-function shapes and all dtype/count limits match CRV-01A/B/C/D exactly. Only naming and position export are new; no second interpolation algorithm is defined by the template, and the numerical, dtype, domain and dependency contract is inherited from the corresponding CRV-01 method.

The samples node is the existing CRV-01 interpolation operation; its values are exported as samples. The positions node is a `core.identity` of the bound new_positions Result, retaining its descriptor, dtype and raw bits, including NaN/Inf/sNaN/negative zero. It has no source positions/values reads and no interpolation validation; it depends only on the requested new_positions region. The bound Result must satisfy the compiled structural contract. Sample requests require CRV-01's finite query and source preconditions.

Authoring static profile=strict/apple_silicon/x86_64 defaults to strict and chooses the corresponding primitive key. Other static parameters are the source's dtype (default Float64) and out_of_domain (default reject, with clamp and the source's defined extrapolation). No implicit low-pass, sample-rate inference, period or anti-alias quality claim is added. The helpers (in `resampling.hpp`) append a CRV-01 samples node and a `core.identity` positions node with collision-free IDs, leave existing workflow exports unchanged and return both node outputs for explicit use or export. They introduce no registered interpolation operation of their own.

## Conceptual DAG

positions -> CRV-01 x; values -> CRV-01 y; new_positions -> CRV-01 query -> samples; new_positions -> `core.identity` -> positions.

## Acceptance

Fixture positions=[0,1,3], values=[0,2,4], new_positions=[2,0.5,2] gives linear samples=[3,1,3] and positions=[2,0.5,2]. PCHIP uses its own exact CRV-01 fixture; multi-function uses the corresponding per-column reference. Test independent positions containing nonfinite bits without running interpolation, and use valid finite queries for samples. All CRV-01 numerical acceptance applies to samples. Verify generated and explicit graph equivalence separately from numerical correctness.

## References

- [Linear single-function template](CRV-11A_resample_linear.md).
- [PCHIP single-function template](CRV-11B_resample_pchip.md).
- [Linear multi-function template](CRV-11C_resample_linear_multi.md).
- [PCHIP multi-function template](CRV-11D_resample_pchip_multi.md).
- [Interpolation family](CRV-01_interpolate.md).
- [Curve category](../curves.md).
- [Uniform low-pass shared contract](CRV-11_uniform_lowpass_contract.md).
- [Nonuniform low-pass shared contract](CRV-11_nonuniform_lowpass_contract.md).
- [uniform hann_sinc](CRV-11E1_lowpass_uniform_hann_sinc.md), [hamming_sinc](CRV-11E2_lowpass_uniform_hamming_sinc.md), [blackman_sinc](CRV-11E3_lowpass_uniform_blackman_sinc.md), [kaiser_sinc](CRV-11E4_lowpass_uniform_kaiser_sinc.md), [gaussian](CRV-11E5_lowpass_uniform_gaussian.md).
- [nonuniform hann_sinc](CRV-11F1_lowpass_nonuniform_hann_sinc.md), [hamming_sinc](CRV-11F2_lowpass_nonuniform_hamming_sinc.md), [blackman_sinc](CRV-11F3_lowpass_nonuniform_blackman_sinc.md), [kaiser_sinc](CRV-11F4_lowpass_nonuniform_kaiser_sinc.md), [gaussian](CRV-11F5_lowpass_nonuniform_gaussian.md).
