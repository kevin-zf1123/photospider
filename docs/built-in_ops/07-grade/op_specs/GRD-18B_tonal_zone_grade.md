---
spec_schema_version: 1
id: GRD-18B
parent_id: GRD-18
function: tonal_zone_grade
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.tonal_zone_grade
---

# GRD-18B: Exposure and offset by tonal zone

Inherits the [family contract](GRD-18_tonal_zones_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.tonal_zone_grade` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `controls: Float64[3,2]` [ev,offset] -> `values: T[S,C]`.

Linear RGB/Gray. Zone convention is GRD-16A. Suggested controls zero. EV uses stops; offset uses the input unit.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The registered key is `grade.tonal_zone_grade`. The ordered required dynamic ports are `input`, `zones` and `controls`; the output port is `values`.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required for raw selection; zero-based component axis within the tensor cell, excluding batch axes. For spatial inputs, `layout.channel_axis` must be present and equal to `axis`. |
| `components` | String | Required with `axis`; one Gray slot or three distinct canonical comma-separated slot indices in R,G,B order. Raw selection asserts linear source coordinates in the caller's declared units. |
| `input_description` | String | Optional existing FMT tensor-description encoding, replacing raw selectors; requires an explicit linear RGB or linear-Y Gray interpretation and reference. |
| `group` | String | Required with `input_description`; selects the Gray/RGB group. Roles resolve component order. |
| `max_work` | Int64 | Optional per-invocation work limit; default 9223372036854775807, minimum zero. |

Raw and description selectors are mutually exclusive. The component axis is explicit, including for Gray. The zones tensor has the same ordered sample axes as the input, with the component-axis extent replaced by three; its dtype is independent of T. Batch dimensions participate in this exact correspondence. The controls table has no batch axes. Alpha and unselected AOV slots retain their original bits.

For spatial inputs selected through `input_description`, the description's component axis must match the input tensor's declared `layout.channel_axis`.

## Mathematics

e=sum(w_z*ev_z)/sum(w_z); o=sum(w_z*offset_z)/sum(w_z); y=x*exp2(e)+o. All-zero controls copy.

### Current native algorithm/profile

For every adjusted sample compute in Float64: `s=(w0+w1)+w2`, `ne=(w0*ev0+w1*ev1)+w2*ev2`, `no=(w0*offset0+w1*offset1)+w2*offset2`, `e=ne/s`, and `o=no/s`. All-zero controls copy without reading zone samples inside the operator. A native `e==0 && o==0` also copies the original source bits. Otherwise form a binary64 `exp2(e)` gain, then `Float64(source)*gain+o`, and convert once to T. The gain is formed before multiplication, including at overflow and underflow; `0*infinity` therefore produces NaN. No clipping, saturation, zone normalization omission or payload-domain scan is applied. Controls are dynamic invocation inputs.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs copy independently of the grading arithmetic.

For this member, the common bypass-only dependency guarantee applies to complete execution tiles. Empty requests consume no payload. A tile containing only unselected components depends solely on the corresponding source coordinates. Spatial tiles cover all component slots, so an alpha-only ROI in an RGBA image with selected RGB still requires source, zones and controls for the enclosing tile. Alpha retains its original bits.

The Plan declares zones and controls whenever a tile contains selected components. All-zero controls skip zone sample reads during operator execution; the declared dependencies and any scheduled upstream producers remain required.

## Acceptance

One-hot shadow weights and shadow EV=1 double selected components. With all controls zero, preserve signed/HDR values.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
