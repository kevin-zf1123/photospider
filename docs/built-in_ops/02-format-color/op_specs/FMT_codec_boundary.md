---
spec_schema_version: 1
id: FMT-codec-boundary
kind: shared_boundary_contract
category: 02-format-color
status: Proposed
implementation_status: not_implemented
clarification_status: image_boundary_confirmed_codec_details_pending
---

# Canonical image planes and I/O codec boundary

This record
assigns responsibilities; it does not implement a codec or select resampling
algorithms. [FMT-16](FMT-16_retired.md) and [FMT-17](FMT-17_retired.md) are retired catalog IDs;
no replacement kernel operators are assigned to them.

## Confirmed internal image contract

- Color planes within one image have the same spatial dimensions and sample
  grid. Y, Cb and Cr have a 1:1:1 plane-size relationship (full-resolution 4:4:4).
  This ratio describes sampling dimensions, not their values or numeric ranges.
- Alpha, when present, is another same-size plane within the image tensor.
  Complete colors remain straight. No heterogeneous-plane carrier or persistent
  external alpha binding is introduced for subsampled video/file data.
- Image layout is explicit structural configuration. The host planar convention
  does not add an implicit payload validator. Current physical execution uses
  distinct dense output tile allocations and prepared input views; source/output
  grids may differ. A consumer requiring a specific layout receives an explicit
  conversion or COPY. Generic arrays retain their own tensor contracts; this
  does not make scalar/vector/table operands into images.
- [ADR 0022](../../../adr/0022-freeze-operation-c-abi-v3.md) D9 (Proposed)
  records the domain boundary; its superseded execution/storage sections do not
  override the current [physical execution contract](../../../kernel-architecture/Compiler-and-Execution.md).
- Same spatial extent is an invariant within an image. It does not require every
  independent image in a workflow to have the same resolution. Geometric
  operators can produce a different output image resolution under their specs.

## Input/output codec responsibilities

Input codecs reconstruct externally subsampled channels and convert external
packing/layout under an explicit import contract before supplying an image
Result. Output codecs consume their declared layout and perform explicitly
selected subsampling, packing and compression. Externally subsampled
4:2:2/4:2:0 planes and packed 1/10/12-bit streams remain codec representations.
Admitted affine interleaved storage does not imply subsampled channel semantics
or waive a consumer's explicit layout requirements.

Codec contracts still need concrete filters/siting, odd-edge behavior, input
validation, region requests, resource limits, cancellation, errors and resulting
explicit color/encoding configuration. Region-limited decoding may need source neighborhoods;
this is a codec dependency obligation rather than a new heterogeneous DAG image
model. This decision neither requires whole-file decoding nor promises a codec
supports random access.

The choice of codec output model and numeric encoding must be explicit in its
own contract. Decoding bytes or reconstructing chroma does not, by itself,
authorize unspecified gamut mapping, tone mapping, ICC/OCIO processing or ACES
input/output rendering. If a codec intrinsically converts a color representation,
its explicit output configuration and numerical contract must describe the result.
A consumer receives that configuration separately; the tensor does not carry it.
Physical layout conversion does not change color interpretation.

## Remaining operator responsibilities

- FMT-11 retains RGB' <-> same-size YCbCr model mathematics.
- FMT-09 owns explicit transfer stages, and FMT-10 owns RGB basis/white stages.
- FMT-06 owns explicit in-graph dtype/interval/code-domain operations. Codec code
  packing does not restore the retired numeric.cast/encode_range interfaces or
  duplicate their old numerical behavior.
- Channel assembly and logical axis/shape operations retain their respective
  FMT/NUM responsibilities while preserving image storage invariants.
- The kernel provides fixed tile allocation and addressing. Grid/layout changes
  use existing explicit structural operations and COPY tasks. Metadata edits
  never convert storage, and this boundary creates no new FMT-17 primitive.

The conversion coverage proposal is in
[FMT model conversion coverage](FMT_model_conversion_coverage.md). No I/O
implementation, plugin-loading mechanism, codec API or new semantic carrier is
selected here. Existing generic codec/serialization helpers in source code are
not evidence that this image input/output codec boundary is implemented.
