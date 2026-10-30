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

This record assigns responsibilities between FMT operators and input/output codecs; it does not define a codec or select resampling algorithms. [FMT-16](FMT-16_retired.md) and [FMT-17](FMT-17_retired.md) are retired catalog IDs with no replacement kernel operators.

## Internal image contract

- Color planes within one image have the same spatial dimensions and sample grid. Y, Cb and Cr have a 1:1:1 plane-size relationship (full-resolution 4:4:4). This ratio describes sampling dimensions, not values or numeric ranges.
- Alpha, when present, is another same-size plane within the image tensor. Complete colors remain straight. There is no heterogeneous-plane carrier or persistent external alpha binding for subsampled video/file data.
- Image layout is explicit structural configuration. A consumer requiring a specific layout receives an explicit conversion or COPY. Generic arrays retain their own tensor contracts; scalar/vector/table operands are not images.
- Same spatial extent is an invariant within an image. It does not require every independent image in a workflow to have the same resolution. Geometric operators can produce a different output resolution under their specs.

## Input/output codec responsibilities

Input codecs reconstruct externally subsampled channels and convert external packing/layout under an explicit import contract before supplying an image Result. Output codecs consume their declared layout and perform explicitly selected subsampling, packing and compression. Externally subsampled 4:2:2/4:2:0 planes and packed 1/10/12-bit streams remain codec representations. Affine interleaved storage does not imply subsampled channel semantics.

Codec contracts define concrete filters/siting, odd-edge behavior, input validation, region requests, errors and the resulting explicit color/encoding configuration. Region-limited decoding may need source neighborhoods; this is a codec obligation rather than a heterogeneous image model. Random access is not promised.

A codec's output model and numeric encoding are explicit in its own contract. Decoding bytes or reconstructing chroma does not authorize gamut mapping, tone mapping, ICC/OCIO processing or ACES rendering. If a codec intrinsically converts a color representation, its explicit output configuration and numerical contract describe the result; a consumer receives that configuration separately. Physical layout conversion does not change color interpretation.

## Operator responsibilities

- FMT-11 owns RGB' <-> same-size YCbCr model mathematics.
- FMT-09 owns transfer stages, and FMT-10 owns RGB basis/white stages.
- FMT-06 owns in-graph dtype/interval/code-domain operations; codec code packing does not duplicate them.
- Channel assembly and logical axis/shape operations retain their NUM/FMT responsibilities.
- Grid/layout changes use explicit structural operations and COPY. Description edits never convert storage.

Conversion coverage: [FMT model conversion coverage](FMT_model_conversion_coverage.md).
