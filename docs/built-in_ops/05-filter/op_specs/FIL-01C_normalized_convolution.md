---
spec_schema_version: 1
id: FIL-01C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-01
function: normalized_convolution
proposed_operation_keys:
- filter.normalized_convolution_strict
- filter.normalized_convolution_accelerated_apple_silicon
- filter.normalized_convolution_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S02
---
# FIL-01C: normalized_convolution

Positive-kernel convolution with participation weights.

Inherits [FIL-01](FIL-01_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.normalized_convolution_strict`, `filter.normalized_convolution_accelerated_apple_silicon`, `filter.normalized_convolution_accelerated_x86_64`.

Inputs are `input`, `mask`, and `kernel`; outputs are `output` and UInt8 `valid` with value 0 or 1. Mask and input must have matching spatial/batch shapes or an explicitly declared singleton component broadcast. Output matches the statistic group shape.

### Parameters

Require axes and anchor, `shape=same`, an explicit boundary (`truncate` is supported), and `empty` chosen from `copy_center`, `zero`, or `error`. Do not expose bias or normalization parameters. Inputs are a raw field, participation mask, and nonnegative kernel. No constructor defaults.

## Mathematics

For each output, let `D=Σ K M`. Callers guarantee `K≥0` and `M∈[0,1]`. If `D>0`, return `RN_t(Σ K M I / D)` and `valid=1`. If `D=0`, apply the required `empty` strategy; successful zero/copy results have `valid=0`, while the explicit error policy reports an empty-support outcome. When `M=0`, I does not participate in arithmetic; when `K=0`, M and I do not participate. Consume the kernel for algorithmic support and normalization without validating its numerical domain. Constant-boundary exterior taps use `M=1`; truncated exterior taps do not participate.

Per output, the kernel and mask determine participating arithmetic terms; runtime zero weights select terms. This profile requires nonnegative kernels and is not a signed-derivative profile.

## Acceptance

Use input `[1,NaN,5]`, mask `[1,0,1]`, and a three-tap unit kernel: the center output is 3 and succeeds. With an all-zero mask, exercise each empty policy. A NaN mask and a negative kernel entry are invalid.

## References

[S01](../research-sources.md#s01), [S02](../research-sources.md#s02).
