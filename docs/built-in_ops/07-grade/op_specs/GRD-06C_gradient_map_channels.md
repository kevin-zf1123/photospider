---
spec_schema_version: 1
id: GRD-06C
parent_id: GRD-06
function: gradient_map_channels
kind: composite_workflow
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8_9
clarification_status: complete
execution: tile
registered_operation_keys:
  - grade.gradient_map_channels_combined
  - grade.gradient_map_channels_compiled
---

# GRD-06C: Generate a model-space image through independent channel LUTs

Select one input channel as a numeric query, construct an independent curve and LUT for each destination color component, assemble those LUTs, and generate a new image in the selected color model. The input channel has purely numeric meaning: its source model may be RGB, XYZ, Lab or any other representation. The destination model defines the output coordinates and their units.

Inherit the [GRD family](GRD-06_gradient_map_contract.md) and [common contract](GRD_common_contract.md) for Result ports, structural checks and tile behavior. This member overrides the family's RGB-only A/B behavior, same-dtype output convention and single final rounding convention with the explicit model, dtype and LUT stage boundaries below.

## Interface

Both proposed registrations have the same ordered dynamic ports:

```text
input: T[S,Cin], start: Float32/64[1], end: Float32/64[1],
x_0: Float32/64[K0], y_0: Float32/64[K0], ...,
x_(D-1): Float32/64[K(D-1)], y_(D-1): Float32/64[K(D-1)]
    -> values: U[S,D], table: U[L,D], axis: Float64[3]
```

`T` and `U` independently select Float32 or Float64. Each dynamic port contains one tensor, as in GRD common. `S` is the ordered complete sample shape after removing the selected input component axis, including any batch coordinates. It has rank 1..7; all extents are positive and every logical input/intermediate/output element count is at most 2^40. Image and table outputs use `photospider.tensor` v1/member `samples`, with ordinary axes and a final component axis; they do not retain the input batch-axis partition. The `axis` output retains NUM-02A's atomic Float64 three-component tuple. `table` and `axis` are independently selectable named outputs.

| Static parameter | Type and domain | Default |
| --- | --- | --- |
| `axis` | Int64 input cell-axis index, excluding any batch prefix, following FMT-01A | Required |
| `query_channel` | Int64 index in `[0,Cin)` on that axis | Required |
| `color_description` | Canonical TensorDescription String for one complete destination tuple at logical channel axis 0, with no alpha | Required |
| `methods` | String containing a JSON array of D strings; each entry `linear` or `pchip` | Required |
| `count` | Int64 L in `[1,1048576]` | Required |
| `dtype` | String `float32` or `float64`, defining U for table and values | `float64` |
| `curve_edges` | String containing a JSON array of D strings; each `reject`, `clamp` or `linear_extrapolate` | All `reject` |
| `lookup_edge` | String `reject`, `clamp` or `linear_extrapolate` | `clamp` |
| `max_work` | Nonnegative Int64 invocation work limit; combined forwards it to budgeted child operations | 9223372036854775807 |

The static input `axis` selector and named output `axis` occupy separate parameter and output namespaces. The two array-valued authoring options use String parameters under the existing operation parameter vocabulary, for example methods=`["linear","pchip","linear"]`. Their JSON shape and enum entries are structural checks. All expanded nodes receive explicit parameters. `color_description` selects D and the ordered model components below. Each channel has its own K, knot positions, ordinates, interpolation method and curve boundary policy. Require 2<=Kc<=65536. Equal ordinate values represent a constant channel. Knot ordering and numerical domains are inherited unchecked premises.

Source selection uses the raw index path of [FMT-01A](../../02-format-color/op_specs/FMT-01A_extract_channel_index.md), with `keepdims=false`. The interface requires no source color description and performs no source-model validation, conversion, luminance extraction, normalization or periodic reduction. A query channel containing Y, Z, H, C, L, R or a precomputed luminance is consumed in its supplied units. Non-query source channels, including alpha and AOVs, are omitted from the new image.

### Destination models

Use the explicit coordinate definitions and descriptions of [CRV-06](../../01-numeric/op_specs/CRV-06_color_ramp.md), restricted to floating color components without alpha:

| Model | D and ordered output components | Coordinate contract |
| --- | --- | --- |
| RGB | 3: R, G, B | Explicit primaries, white and transfer; finite signed/HDR coordinates |
| XYZ | 3: X, Y, Z | Relative white-Y=1, explicit reference white; finite extensions |
| CIELAB | 3: l, a*, b* | l=L*/100, explicit reference white; finite extensions |
| CIELCh(ab) | 3: l, C*, h | l=L*/100, C*>=0, explicit white and floating hue unit |
| OKLab | 3: L, a, b | Native coordinates, fixed D65; finite extensions |
| OKLCh | 3: L, C, h | Native L, C>=0, fixed D65 and floating hue unit |
| CMYK | 4: C, M, Y, K | Normalized ink coordinates in [0,1], explicit printing-profile identity |
| HSL | 3: H, S, L | Explicit underlying RGB description; finite extended S/L and floating hue unit |
| YCbCr NCL | 3: Y', Cb, Cr | Explicit underlying encoded RGB and NCL coefficients; unbiased floating coordinates |

CIELAB and OKLab, and CIELCh and OKLCh, are distinct model choices. CMYK inherits the immutable ICC output-profile resource and structural validation of [CRV-06B](../../01-numeric/op_specs/CRV-06B_color_ramp_cmyk.md); interpolation uses supplied ink coordinates without executing a profile transform. Model coordinate validity is an unchecked caller premise on the consumed controls and generated coordinates, including LUT samples and extrapolated outputs. In particular, a boundary policy does not automatically repair negative chroma or out-of-range ink values.

Every curve operates directly on its declared component coordinates. RGB with linear transfer produces a linear-RGB-coordinate LUT; RGB with an encoded transfer produces an encoded-RGB-coordinate LUT. This is an explicit exception to CRV-06A's decode/interpolate/encode formula: GRD-06C uses scalar CRV/NUM interpolation for each channel and performs no transfer conversion. Other model coordinates likewise retain their declared representation throughout baking, lookup and output. The Result carries structural metadata and numerical samples; the caller supplies the destination description to subsequent consumers under the existing FMT contract.

### Hue representation

Polar hue is Float32/Float64 in `radian` or `pi_multiple`, identified by `color_description`, following the floating representations of [CRV-06D](../../01-numeric/op_specs/CRV-06D_color_ramp_cielch.md), [CRV-06F](../../01-numeric/op_specs/CRV-06F_color_ramp_oklch.md) and [CRV-06G](../../01-numeric/op_specs/CRV-06G_color_ramp_hsl.md). Controls, baked hue samples and output use the same unit. Preserve the supplied sign and winding count over the full finite floating domain, including values outside one turn. Hue participates at zero chroma or saturation. Interpolation uses the original ordinate differences; query lookup also uses the original query value. There is no modulo, shortest-arc substitution, endpoint wrapping or achromatic hue replacement. Rational-pi numerator/denominator ports and implicit hue-unit conversion are outside this floating interface.

## Mathematics

1. Extract `q[s]=input[s,query_channel]` by bit copy in T. Input selection is independent of the output color model.
2. Sample a common Float64 axis from `start` to `end`, with L=`count`, using [NUM-02A](../../01-numeric/op_specs/NUM-02A_linspace.md). The published axis is `[start,end,step]`; L=1 uses `[start,start,+0]` and omits the end payload dependency. Both ascending and descending grids follow the source and LUT-consumer contracts.
3. For each component c, bake `f_c(x_c,y_c)` at that common grid using [CRV-04C](../../01-numeric/op_specs/CRV-04C_bake_lut1d_linear.md) or [CRV-04D](../../01-numeric/op_specs/CRV-04D_bake_lut1d_pchip.md), with its own `curve_edges[c]`. Publish each scalar table column in U before the next stage.
4. Assemble the columns by exact copies with [FMT-02A](../../02-format-color/op_specs/FMT-02A_assemble_channels.md), giving `table[j,c]=f_c(grid[j])` after the specified U conversion. Every column uses the same L and axis; knot counts and control locations may differ between columns.
5. Apply [CRV-05C](../../01-numeric/op_specs/CRV-05C_apply_lut1d_vector.md) to the extracted scalar `q`, assembled `table` and `axis`, with output dtype U and `lookup_edge`. The same query selects every table column, producing `values[S,D]` in the destination model. Its reference composition explicitly replicates q into D channels and invokes CRV-05B; direct vector lookup shares the query without requiring that replicated tensor.

The baking method and final LUT interpolation are separate stages. Final lookup uses CRV-05's piecewise-linear formula between baked entries, even when the source curve is PCHIP. L is explicit and finite, so the LUT function may differ from direct evaluation of the source curve. Lookup uses the CRV-05 native normalized-axis formula; exact normalized endpoint positions directly select the stored endpoint entry. Interior grid positions retain that native formula and its rounding, rather than assuming strict-profile knot reconstruction. Endpoint selection, query clamping, linear extrapolation, singleton behavior and signed-zero rules follow the corresponding CRV/NUM node. Query clamping constrains the query coordinate; it does not clip output component values to a nominal model range.

Production arithmetic follows the native `fast` profiles of NUM-02A, CRV-01 and CRV-05. FMT extraction and assembly are exact copies. Preserve the Float64 sampling boundary, U table boundary and U output boundary in both registrations. In particular, generating an image by directly evaluating its control curves would change the declared LUT function. The strict reference is the same staged composition with strict CRV/NUM equations, including each publication rounding; it is a reference for comparison, not an additional registration.

All selected finite-value, ordering, representability and consumer-axis premises are inherited from the contributing CRV/NUM contracts. For L>1 the baked grid must satisfy CRV-05's strictly ordered rounded-grid and axis-consistency premises; a valid sampler alone does not establish a valid LUT axis. L=1 follows CRV-05's singleton rules, including its lack of image-query payload dependency and reject-domain premise. There is no automatic value-range clipping, gamut mapping or conversion to RGB after lookup.

## Registration variants

| Registered operation key | Required realization |
| --- | --- |
| `grade.gradient_map_channels_combined` | Compose the named stages using existing nodes/task groups or explicit existing IR control flow, with intermediate LUT columns, the assembled table and query edges represented in the computation. |
| `grade.gradient_map_channels_compiled` | Provide an independently compiled fixed binary operation implementing the same staged function and named outputs through the existing fixed invocation interface. |

Both keys are required registrations with the same ports, parameters, destination-model support, schemas and acceptance. A combined entry must execute the composition rather than alias the compiled implementation. A compiled entry must execute its compiled implementation rather than expand into the combined entry. Stage storage and internal task grouping may differ while retaining published rounding boundaries and observable dependencies. Selection between the two keys is explicit. Neither registration changes kernel interfaces or relies on new IR instructions, ABI layouts or runtime dependency discovery.

## Fast dependencies and implementation status

The current combined profile lowers the actual sampling, per-component curve, extraction, assembly and vector-lookup operations into existing fixed CPU task groups. The terminal lookup writes public `values` tiles directly; independently exported table and axis tiles retain their own output storage. The compiled profile evaluates the same stages in a fixed invocation, baking the requested component columns into invocation-local scratch. It uses bounded batches for Float64 grid generation and curve evaluation, converts each baked value to U, and only then widens the stored table for shared vector lookup. Each output task uses the current Run inputs; it introduces no numerical cache between Runs.

| Stage | Dependency | Status |
| --- | --- | --- |
| Shared sampling grid | `numeric.linspace_fast` (NUM-02A) | Existing implementation |
| Linear component curve | `curve.interpolate_linear_fast` (CRV-01A), through CRV-04C | Existing implementation |
| PCHIP component curve | `curve.interpolate_pchip_fast` (CRV-01B), through CRV-04D | Existing implementation |
| Direct scalar-to-vector LUT lookup | `curve.apply_lut1d_vector_fast` (CRV-05C) | Implemented native fast |
| Explicit reference composition for vector lookup | `curve.apply_lut1d_channels_fast` (CRV-05B), after query replication | Existing implementation |
| Source extraction and LUT assembly | FMT-01A and FMT-02A exact copy paths | Existing implementation |

NUM-02A already supplies the required native sampling path. This workflow requires no additional NUM arithmetic primitive, RGB transfer operation or CRV-06 color-ramp interpolation key: its curves and LUT values are directly expressed in the selected destination coordinates. The compiled implementation must preserve the same stage semantics even when it integrates those computations into its own fixed binary.

## Behavior

`values` generates a fresh model-space image with D components and the source's non-component sample dimensions. It replaces no source slots and copies no unrelated source channels. Source edits outside the selected query channel do not affect values. Alpha construction or preservation, output conversion and effect mixing are separate compositions.

The mathematical image support is the query at the same sample coordinate plus the relevant LUT data. Table generation additionally depends on the component's control curve and sampling endpoints. Full auxiliary tables/control arrays may be declared as static support supersets. A table-only request has no image payload dependency; an axis-only request has no image or curve-control payload dependency. L=1 may eliminate the input-query dependency while retaining the baker's own control/start dependencies. Empty outputs have no payload dependencies. Source and control changes are evaluated from the current Run's inputs; the specification introduces no implicit cross-Run LUT cache.

Whole-image, nonzero ROI and tiled requests agree at identical global coordinates under the native acceptance criteria. Structural parameter/schema checks follow the common contract. Numerical and model-domain premises remain unchecked, including the generic CRV/NUM policies selected for each stage.

## Acceptance

Compare combined and compiled against the same staged reference and explicit node composition. Use the GRD native analytic-fixture tolerances for computed values; extraction, assembly and direct selection from the same stored table compare bits. Registration alone does not establish numerical or runtime conformance.

- **Identity and independent control locations:** RGB with linear transfer, L=3, start=0, end=1. R controls x=[0,1], y=[0,1]; G controls x=[0,0.5,1], y=[0,1,2]; B controls x=[0,1], y=[1,0]. All methods linear. Query 0.25 gives `(0.25,0.5,0.75)`. R reproduces the query in range. With lookup clamp, queries -1 and 2 give `(0,0,1)` and `(1,2,0)`; G remains 2 at the upper endpoint.
- **Input model independence and new shape:** apply that fixture to index 1 of a five-channel input. Changing all other input channels leaves output unchanged, including NaN payloads in unused slots. Output has exactly three components. The input can supply arbitrary numeric channel meanings without a source color description.
- **Encoded RGB coordinates:** sRGB-described black-to-white component controls, L=2 and query 0.5 give `(0.5,0.5,0.5)`, because the component LUTs interpolate encoded ordinates directly.
- **PCHIP baking boundary:** for one channel use x=[0,1,2], y=[0,1,4], start=0, end=2, L=3 and PCHIP. The table is `[0,1,4]`; lookup at 0.5 gives 0.5. Direct PCHIP evaluation would give 0.3125 and is not this LUT result. Other channels may independently use linear interpolation.
- **Unwrapped hue:** OKLCh with constant L=0.5, constant C=0, pi-multiple hue controls 0 and 4 at x=0 and 1, and L=3, returns hue 2 at query 0.5 and 4 at query 1. Hue controls 1.75 and 0.25 yield midpoint 1. Repeat in radians with numerical endpoints 0 and 8, yielding midpoint 4. Selecting an input hue as query does not introduce periodic lookup.
- **CMYK output:** valid described CMYK controls from `(0,0,0,0)` to `(1,0.5,0,0.25)`, L=2 and query 0.5 yield `(0.5,0.25,0,0.125)` in a four-component image, using the supplied profile identity.
- **LUT policies and requests:** cover both floating dtypes, negative/HDR coordinates where the model permits them, descending grids, per-curve boundary policies, lookup extrapolation within model premises, exact hits and signed zeros, L=1, image/table/axis-only selections, nonzero ROI and cross-tile positions. Confirm that changing controls between Runs changes the table and image in both variants.

## Conceptual DAG

```text
start, end -> NUM-02A grid/axis
grid + x_c/y_c -> CRV-01A/B per component -> FMT-02A -> table[L,D]
input -> FMT-01A raw channel extraction -> query[S]
query + table + axis -> CRV-05C fast vector lookup -> values[S,D]
```

The diagram specifies the combined computation and the compiled version's staged numerical reference. CRV-04C/D describe the grid-plus-curve baking branches. Destination interpretation follows CRV-06 coordinate definitions; scalar interpolation and LUT lookup follow CRV-01/04/05.
