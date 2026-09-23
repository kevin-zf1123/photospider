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

The maintainer confirmed the following image boundary on 2026-09-23, superseding
the heterogeneous-plane alternatives in the FMT-09..18 scope review. This record
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
- Every kernel image operator obeys the accepted planar storage contract:
  common DAG tile geometry, whole-image virtual span, row/edge padding, page
  alignment and explicit backing preparation. Raw/override cannot waive the
  physical storage contract. Generic non-image arrays retain their own tensor
  contracts; this does not make scalar/vector/table operands into images.
- Same spatial extent is an invariant within an image. It does not require every
  independent image in a workflow to have the same resolution. Geometric
  operators can produce a different output image resolution under their specs.

## Input/output codec responsibilities

Input codecs reconstruct externally subsampled channels and convert external
packing/layout before publishing a canonical image tensor. Output codecs consume
canonical planar tensors and perform any explicitly selected subsampling,
packing and compression at the external boundary. External interleaved buffers,
4:2:2/4:2:0 planes and packed 1/10/12-bit streams are codec representations,
not alternative kernel image layouts or FMT operation outputs.

Codec contracts still need concrete filters/siting, odd-edge behavior, input
validation, region requests, resource limits, cancellation, errors and resulting
color/encoding metadata. Region-limited decoding may need source neighborhoods;
this is a codec dependency obligation rather than a new heterogeneous DAG image
model. This decision neither requires whole-file decoding nor promises a codec
supports random access.

The choice of codec output model and numeric encoding must be explicit in its
own contract. Decoding bytes or reconstructing chroma does not, by itself,
authorize unspecified gamut mapping, tone mapping, ICC/OCIO processing or ACES
input/output rendering. If a codec intrinsically converts a color representation,
its declared output metadata and numerical contract must describe the result.
Physical layout conversion does not change color interpretation.

## Remaining operator responsibilities

- FMT-11 retains RGB' <-> same-size YCbCr model mathematics.
- FMT-09 owns explicit transfer stages, and FMT-10 owns RGB basis/white stages.
- FMT-06 owns explicit in-graph dtype/interval/code-domain operations. Codec code
  packing does not restore the retired numeric.cast/encode_range interfaces or
  duplicate their old numerical behavior.
- Channel assembly and logical axis/shape operations retain their respective
  FMT/NUM responsibilities while preserving image storage invariants.
- The kernel provides allocation and addressing. There is no image-layout op
  permitting an interleaved image, different per-node tile size or metadata-only
  bypass of planar requirements.

The conversion coverage proposal is in
[FMT model conversion coverage](FMT_model_conversion_coverage.md). No I/O
implementation, plugin-loading mechanism, codec API or new semantic carrier is
selected here. Existing generic codec/serialization helpers in source code are
not evidence that this image input/output codec boundary is implemented.
