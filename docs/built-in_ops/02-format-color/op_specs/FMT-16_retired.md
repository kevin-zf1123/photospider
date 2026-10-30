---
spec_schema_version: 1
id: FMT-16
kind: retired_catalog_entry
category: 02-format-color
status: Retired
implementation_status: not_applicable
clarification_status: retirement_confirmed
---

# FMT-16: retired chroma sampling family

FMT-16 is retired. The number is preserved; it is not reused and has no operation or alias.

External chroma subsampling and reconstruction, including filters, siting, odd extents and codec-specific support mapping, belong to input/output codecs. RGB' <-> full-resolution YCbCr mathematical conversion remains in FMT-11. Transfer and numeric encoding remain with FMT-09 and FMT-06.

All internal image channels have the same spatial shape and sample grid; Y/Cb/Cr use a 1:1:1 plane-size relationship (4:4:4), with any alpha inside the same tensor. See the [shared codec boundary](FMT_codec_boundary.md).
