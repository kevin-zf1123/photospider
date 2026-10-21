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
cell-axis coordinates, layouts, explicit interpretation, demand, ownership and
errors. The FMT specification remains Proposed. This registered primitive
copies one indexed cell-axis element to each requested output coordinate.

## Ports and parameters

Input `input` and output `values` are Results with one tensor member and no
fields. The output preserves source dtype, schema id, tensor key, batch axes and
publication policy. It removes the selected cell axis by default, or retains it
with extent one when `keepdims=true`. The batch prefix is never changed.

The direct node requires static `index`, `keepdims` and `layout` parameters and
accepts optional `axis` and `input_description`. `axis` is an index into the
tensor descriptor, not the complete sample shape. `index` is zero-based and must
be less than that cell-axis extent. `axis` may be omitted only when the supplied
`input_description` identifies a `channel_axis`; without a description an
explicit axis is required. An axis that disagrees with the description fails.

`layout` is `auto`, `view` or `materialize`. The public `ChannelExtractOptions`
has no description, keepdims false, auto layout and strict CPU profile by
default. The three registry profiles use the same exact bit-copy mapping; there
is no GPU implementation.

## Mapping

For complete sample coordinates, preserve each batch coordinate and fix the
cell coordinate on axis `batch_rank + axis` to `index`. Remove the cell axis
from the output shape when keepdims is false; otherwise replace its extent with
one. Copy every requested value exactly, without arithmetic or sample-domain
validation. NaN payloads, infinities, signed zero and integer extrema are
preserved.

The output carries no description of the selected component. A later consumer
that needs the component's name, role, unit or interpretation receives it as
its own explicit parameter.

## Data demand and storage

Each selected output tile lowers to a fixed CPU task. Its mapped source rectangle is closed over the source's own tile grid; atomic/spatial closure can include extra channels in the physical input views. Actual copying reads only selected coordinates and preserves raw bits. Preparation fixes exact read slots and output geometry, with zero scratch and no runtime dependency discovery.

Auto/materialize copy into independent dense output storage. Nonempty view requests return InvalidArgument/InvalidDomain with ViewUnavailable and have only a schema dependency. Empty performs static checks without a payload task. Legal source views may have signed/zero strides or unaligned offsets. Output topology follows the family projection rules, while output storage remains independently owned. Cancellation is checked between at most 256 copied samples; failed calls publish no partial result.

## Example and validation

Use a UInt8 `[2,2,4]` tensor with channel order B/A/R/G and row-major samples
`[30,25,10,20, 31,50,11,21, 32,75,12,22, 33,99,13,23]`. With axis 2 and index 2,
the red output is `[[10,11],[12,13]]`. Reading its second row returns `[12,13]`,
corresponding to source coordinates `[1,0,2]` and `[1,1,2]`. This is an analytic
acceptance case for a temporary public Result check.
