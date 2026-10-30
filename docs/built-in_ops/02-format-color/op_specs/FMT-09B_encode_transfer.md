---
spec_schema_version: 1
id: FMT-09B
parent_id: FMT-09
function: encode_transfer
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.transfer_encode_strict
  - color.transfer_encode_accelerated_apple_silicon
  - color.transfer_encode_accelerated_x86_64
---

# FMT-09B: encode transfer

Inherits the [FMT-09 contract](FMT-09_transfer_contract.md) and [scalar definitions](FMT-09_transfer_math.md). This direction has strict, accelerated Apple Silicon and accelerated x86-64 CPU keys.

## Interface

One input Result with one tensor member produces one `values` output Result containing one tensor. The member uses Float32 or Float64, retains its dtype, shape and axis positions, and preserves Result schema identity, tensor key and batch axes. The output carries no description and `atomic_trailing_axes` is zero. Complete sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. Axis indices refer to cell axes and exclude the Result batch prefix. A target curve is always explicit; gamma, BT.2020 variant and BT.1886 black/white parameters follow the family table. The explicit source description identifies a compatible linear quantity. An encoded source is rejected rather than encoded again, even when its existing transfer matches the chosen target.

## Mathematics

For each participating q, evaluate RN_dtype(E(input[q])). Semantic callers establish the family domain and finite-output preconditions; raw uses the full IEEE formula. Linear and gamma=1 obey the bit-copy identity. Unselected samples copy exactly. No alpha multiplication, hidden-color clearing, basis/white conversion or range normalization occurs. Integer dtype is converted before B in all modes. Non-native floating code ranges require explicit decoding in semantic mode; raw may apply E directly to their stored numeric values.

Encode produces values for the selected curve without changing primaries or white. No output description is attached. An ACEScct curve on another RGB basis does not imply a complete ACEScct space; the next consumer receives its own explicit description.

## Behavior

Semantic PQ inputs are in [0,10000] cd/m^2, BT.1886 inputs in [Lb,Lw] and HLG inputs in [0,1]. Reference compatibility is checked statically. The caller establishes sample validity for every participating coordinate. Auto/materialize write independent dense tiles. Nonidentity forced view fails statically; a nonempty identity view fails ViewUnavailable. Empty requests retain static checks and emit no payload.

## Acceptance

1. Gamma=2: [-4,-0,0.25,4] -> [-2,-0,0.5,2] exactly. Gamma=1 raw copies all bits; semantic identity requires finite participating samples.
2. sRGB: the decimal breakpoint is not an exactly representable Float32/64 sample. Test each dtype's bit patterns adjacent to the exact rational threshold with an independent branch oracle. For example Float32 0x3b4d2e1c denotes approximately 0.0031308000907301903, lies above the threshold, and takes the nonlinear segment; its strict output is 0x3d25aece, not the rounded linear anchor 0x3d25aed5. Test negative counterparts and unclipped values above 1.
3. 709/2020: test beta and its neighbors for each selected coefficient variant.
4. BT.1886 with Lb=1,Lw=100: E(1)=0, E(100)=1 exactly. Semantic E(0) is outside its input precondition; raw E(0) gives -b using the selected exact black/white parameters.
5. PQ values outside [0,10000] violate the semantic input precondition. Raw values retain the formula, with no extra clipping.
6. HLG: E(0)=0. Compare exact input values on either side of 1/12 to the high-precision reference; rounded input 1/12 is not the exact breakpoint.
7. ACEScc: -1 and either zero encode to RN_dtype((-16+9.72)/17.52). E(2^-15)=RN_dtype((9.72-15)/17.52). Positive values above 65504 encode normally, although a subsequent A cap prevents recovery.
8. ACEScct: E(0)=RN_dtype(B); E(-1)=RN_dtype(B-A). Test Xbreak and neighbors. There is no low-end clipping or sRGB-style odd extension.
9. Encoded-source rejection, incompatible reference/unit rejection, different explicit descriptions and raw calls are distinct fixtures. A P3 group with linear relative values may use sRGB E without changing basis.
10. Full and sparse output tile selections; bypass-only tiles copy bits. Alpha=0 does not waive participating color preconditions.

An explicit D -> basis/reference stage -> E workflow composes these operations. FMT-06 and an I/O codec remain explicit stages when external integer or file output is required.
