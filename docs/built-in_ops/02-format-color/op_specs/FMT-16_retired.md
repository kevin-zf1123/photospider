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

The maintainer confirmed retirement of this catalog ID on 2026-09-23. Preserve
the number for traceability; do not reuse it, renumber subsequent families or
create an FMT-16 operation/compatibility alias.

External chroma subsampling and reconstruction, including filters, siting,
odd extents and codec-specific support mapping, belong to input/output codecs.

RGB' <-> full-resolution YCbCr mathematical conversion remains in FMT-11.
Transfer and numeric encoding retain FMT-09 and FMT-06 responsibilities.

All internal image channels have the same spatial shape and sample grid;
Y/Cb/Cr use a 1:1:1 plane-size relationship (4:4:4), with any alpha inside the
same tensor. Every kernel image operator obeys canonical planar storage,
including calls using raw/override. External codec representations do not
introduce a second internal image layout or heterogeneous-plane carrier.

The [shared codec boundary](FMT_codec_boundary.md) records the selected scope.
Its detailed codecs, algorithms, interfaces, resources and regional contracts
still require separate specifications. No codec implementation is delivered by
this retirement record. This catalog decision is distinct from the already
recorded [old implementation removal](FMT_legacy_retirement.md).
