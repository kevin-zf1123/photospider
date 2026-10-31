---
spec_schema_version: 1
id: FMT-10B
parent_id: FMT-10
function: xyz_to_rgb
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyz_to_rgb_strict
  - color.xyz_to_rgb_fast
  - color.xyz_to_rgb_accelerated_apple_silicon
  - color.xyz_to_rgb_accelerated_x86_64
---

# FMT-10B: XYZ to linear RGB

Inherits the [FMT-10 family](FMT-10_rgb_basis_contract.md) and [exact matrix definitions](FMT-10_basis_math.md). The keys are the strict, accelerated Apple Silicon and accelerated x86-64 CPU profiles.

## Interface

One input Result with one Float32 or Float64 tensor produces one `values` output Result containing one tensor. The output preserves Result schema identity, tensor key, logical shape and batch axes; it carries no description and sets `atomic_trailing_axes` to zero. Complete sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. A complete XYZ group is selected in semantic mode, or an explicit ordered raw triple. Target RGB basis geometry is required. The exact normalized M is constructed and M^-1 used; there is no transfer, adaptation, exposure or gamut constraint.

Semantic white_handling defaults to require_match: the input XYZ white matches the target RGB basis white. Explicit preserve_xyz allows a mismatch while using the same target inverse matrix; it does not map source white to target white. To adapt, C is called first. Raw B ignores source color interpretation and accepts no white_handling field; explicit target geometry remains valid.

## Mathematics

For each transformed row i, the output is RN_dtype(sum_j inverse(M)[i,j]*XYZ[j]+0). A nonidentity row reads all source X/Y/Z; semantic callers guarantee finite consumed triples and rounded outputs; raw applies NUM's full three-term behavior. Source unit/reference scale stays unchanged. Linear R/G/B are written into the original X/Y/Z slots. The target basis and source scale define the numerical interpretation; callers pass that configuration downstream explicitly. Neither path attaches a description.

## Behavior

Native exact-I copy exceptions, nonidentity tuple/bypass support and auto/view/materialize follow the family. An explicit preserve_xyz choice does not authorize a view of a nonidentity inverse matrix. Default white mismatch and incompatible source/model/reference are preflight errors; geometry and source assertions are checked even for Empty/bypass-only requests. Negative/out-of-gamut RGB is legal; finite semantic input/output is an unchecked caller precondition.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-10_rgb_basis_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

1. The A custom-axis basis with white=(0.25,0.25) has inverse diag(1,1,0.5). XYZ [1,-2,6] maps to RGB [1,-2,3] exactly in both dtypes.
2. For floating XYZ samples near each preset white, compare to exact inverse-M evaluation. A nonrepresentable rational W cannot be injected as an exact floating sample to claim B(W)=[1,1,1] without input rounding.
3. Input white Ws=(1,1,2) from xy=(0.25,0.25), target custom-axis basis white Wt=(2,1,1) from xy=(0.5,0.25): default require_match rejects. With explicit preserve_xyz, input XYZ [1,1,2] yields RGB [0.5,1,2] exactly, showing that source white need not become neutral under this policy.
4. With C mapping Ws to Wt before the target B, the representable white input [1,1,2] yields [1,1,1] for the chosen fully adapted white. Without C it is rejected.
5. A diagonal inverse row still consumes its zero-coefficient peers. A NaN Y makes a retained raw R NaN and is outside the semantic input precondition. A request for only alpha copies exactly; no color evaluation is needed.
