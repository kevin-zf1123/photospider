---
spec_schema_version: 1
id: FMT-01A
parent_id: FMT-01
function: extract_channel_index
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
operation_keys:
  - channel.extract_index_strict
  - channel.extract_index_accelerated_apple_silicon
  - channel.extract_index_accelerated_x86_64
---

# FMT-01A: extract a cell axis by index

Inherit [FMT-01](FMT-01_channel_extraction_contract.md) for the Result schema,
cell-axis coordinates, layouts, metadata projection, demand, ownership and
errors. The FMT specification remains Proposed. This registered primitive
copies one indexed cell-axis element to each requested output coordinate.

## Ports and parameters

Input `input` and output `values` are Results with one tensor member and no
fields. The output preserves source dtype, schema id, tensor key, batch axes and
publication policy. It removes the selected cell axis by default, or retains it
with extent one when `keepdims=true`. The batch prefix is never changed.

The direct node requires static `axis`, `index`, `keepdims`, `layout`, and
`metadata_mode` parameters. `axis` is an index into the tensor descriptor, not
the complete sample shape. `index` is zero-based and must be less than that
cell-axis extent. `axis` may be omitted only when the effective
`photospider.tensor-description` identifies exactly one `channel_axis`; raw mode
always needs an explicit axis. A direct axis assertion that disagrees with
respect-mode metadata fails.

`metadata_mode` is `respect`, `raw` or `override`; `metadata_override` is
required exactly in override mode. `layout` is `auto`, `view` or `materialize`.
The public `ChannelExtractOptions` defaults to respect, keepdims false, auto
layout and strict CPU profile. The three registry profiles use the same exact
bit-copy mapping; there is no GPU implementation.

## Mapping and metadata projection

For complete sample coordinates, preserve each batch coordinate and fix the
cell coordinate on axis `batch_rank + axis` to `index`. Remove the cell axis
from the output shape when keepdims is false; otherwise replace its extent with
one. Copy every requested value exactly, without arithmetic or sample-domain
validation. NaN payloads, infinities, signed zero and integer extrema are
preserved.

If the selected axis is the described channel axis, project the selected
component's name, role, unit and applicable interpretation. A retained singleton
axis receives a one-entry channel table. After squeezing it, carry a component
description without claiming the result still has a channel axis. Other color
groups that depend on the removed channel axis are not projected as complete
groups. A/B do not promise to retain arbitrary opaque annotations.

## Data demand and storage

For an output footprint Q, the Result continuation requests only the mapped
source support and the source descriptor. Its Need role mask is 9: Data (1) and
Descriptor (8), with no Validation or Control role. Dependency-v2 maps dirty
selected-channel samples back to the same non-channel output coordinates;
changes to unselected channels do not dirty this output. An Empty request
publishes an empty Result without reading payload.

Generic Result tensors can expose valid positive-, negative- or zero-stride
views. The mapper can preserve authorized backing partitions, including views
with multiple owners. Selecting a spatial tensor's declared channel axis can
retain the source physical owner and channel plane. Slicing a spatial height or
width axis materializes in auto mode and fails under forced view. Materialized
copies cover only the requested output and check cancellation within runs of at
most 256 samples. The primitive disables Result caching.

## Example and validation

Use a UInt8 `[2,2,4]` tensor with channel order B/A/R/G and row-major samples
`[30,25,10,20, 31,50,11,21, 32,75,12,22, 33,99,13,23]`. With axis 2 and index 2,
the red output is `[[10,11],[12,13]]`. Reading its second row returns `[12,13]`,
corresponding to source coordinates `[1,0,2]` and `[1,1,2]`. This is an analytic
acceptance case for a temporary public Result check.
