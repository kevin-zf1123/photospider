---
spec_schema_version: 1
id: FMT-17
kind: retired_catalog_entry
category: 02-format-color
status: Retired
implementation_status: not_applicable
clarification_status: retirement_confirmed
---

# FMT-17: retired physical image layout conversion family

FMT-17 is retired. The number is preserved; it is not reused and has no operation or alias.

External planar/interleaved/packed conversion and bit packing belong to input/output codecs. Logical axis/shape and channel transformations remain in their NUM/FMT families.

All internal image channels have the same spatial shape and sample grid; Y/Cb/Cr use a 1:1:1 plane-size relationship (4:4:4), with any alpha inside the same tensor. See the [shared codec boundary](FMT_codec_boundary.md).
