# Image operations

## Scope and current availability

The built-in image operations listed below execute through typed Result image slots. Each operation declares its supported schema, region rule, backend and publication behavior; registration alone does not imply support for every structural image layout.

## Production source and runtime status

| Family | Current source and availability | Current Result image status |
| --- | --- | --- |
| FMT alpha | `alpha_operations.cpp`, `alpha_common.hpp`, and `alpha_authoring.cpp` are built; `photospider/ops/format/alpha.hpp` is installed. | Nine native Result ABI 3 keys register associate, unassociate and set across three CPU profiles. `extract_alpha` and `remove_alpha` remain helper compositions over registered operations. |
| FMT channel extraction | `channel_extraction.cpp` registers the six `channel.extract_index_<profile>` and `channel.extract_named_<profile>` CPU Result operations; `format::split_channels` expands index nodes. The public `photospider/ops/format/channel.hpp` header is installed separately. | Exact component access for generic and spatial Result tensors with dependency-v2 support. This is a tensor operation, not a pixel conversion. |
| FMT literal-like fill | `channel_literal_like.cpp` registers three `channel.literal_like_<profile>` Result CPU keys. | One input Result; Descriptor role 8 only, no source sample reads; repeats same-dtype raw bits across requested output samples. It can support the opaque FMT-05B lowering path, but is not `extract_alpha`. |
| FMT scalar literal | `channel_assembly.cpp` registers three `channel.scalar_literal_<profile>` Result CPU keys. | No-input Whole primitive that publishes prepared native bits as one generic shape `[1]` tensor for FMT-03 scalar sources. |
| FMT channel assembly/editing | `channel_assembly.cpp` registers A/B/C CPU Result keys; installed assembly/editing headers provide A/B/C and swizzle/replace helpers. | Result operation ABI 3; single-tensor inputs/outputs, seven exact-bit dtypes, bounded sample counts, descriptor checks for all inputs and mapped Data support. |
| FMT metadata assignment | `metadata_assignment.cpp` registers `metadata.assign_<profile>` keys; `format::remove_metadata` is the lowering helper. The public `photospider/ops/format/metadata.hpp` header is installed separately. | Result operation ABI 3; one tensor member and no fields. Preserves schema, tensor key, batches, layout and sample bits. This is a generic Result tensor operation, not an image-specific operation family. |
| FMT RGB basis conversion | `rgb_basis.cpp` and `rgb_basis_math.cpp` register A/B/C CPU Result keys; `photospider/ops/format/rgb_basis.hpp` is installed. | Nine Result ABI 3 keys across three CPU profiles. `format::convert_linear_rgb` transactionally composes registered stages and adds no D key. This is a generic tensor operation, not an image slot operation. |
| FMT model conversion | `model_conversion.cpp`, `model_math.cpp`, and `model_simd.cpp` register the FMT-11 Result operations; `photospider/ops/format/model_conversion.hpp` provides graph helpers. | 57 native keys cover A-R/T across three CPU profiles; FMT-11S lowers to the registered MASK threshold operation. |
| FMT numeric conversion and transfer | `numeric_conversion.cpp` registers `numeric.convert_format_strict`; `transfer.cpp` registers `color.transfer_encode_<profile>` and `color.transfer_decode_<profile>`. `photospider/ops/format/transfer.hpp` is installed separately. | Both are generic Result tensor operations, not image-specific families. Numeric conversion supports same-coordinate dtype/range conversion; transfer supports scalar curve conversion for selected semantic RGB/Gray groups or explicit raw components. |
| Numeric LUT and structured LUT Results | `lut3d_bake_geometry.cpp` provides Whole numeric Value geometry operations. In `lut3d_bake_results.cpp`, Pack and Measure publish structured Results; Unpack and Gate publish Values. `lut1d_application.cpp`, `lut3d_application.cpp`, `lut_apply_1d.cpp`, and `field_apply_lut_1d.cpp` consume numeric Value/table contracts. | These numeric and non-image structured operations are not Result image implementations. |

## Native alpha Result operations

The installed `photospider/ops/format/alpha.hpp` appends FMT-04A/B and FMT-05A
nodes to ordinary workflows. Their nine native keys use Result ABI 3; FMT-05B/C
remain authoring compositions over existing extraction, literal-like fill,
mapped assembly and metadata assignment keys. These are generic single-tensor
operations, not special `photospider.image` slots. They preserve batch prefixes,
use cell-relative axes, and enforce full sample rank at most 8 and count at most
2^40. Inputs must contain one tensor and no fields; Float32/Float64 same-dtype
arithmetic is used, while extraction/removal and component copies retain the
seven supported raw-bit dtypes.

 Raw association has arithmetic Data without semantic-
domain Validation. FMT-05A requests selected source colors as Data and
Validation, adds Validation-only support from the new alpha source for those
colors, and requests Data plus Validation for the published alpha channel. All
connected inputs receive Descriptor support; these operations use no Control.

FMT-04 associate/unassociate always materialize and expose no layout parameter.
Among the native alpha operations, only FMT-05A Set accepts a layout policy. A generic Set view requires the full
declared map to prove one `CpuStorage` owner and one affine relation; spatial Set
views are limited to internal identity. Set `auto` materializes only on physical
`ViewUnavailable`. Materialization publishes transactionally under Root budgets
and cancellation; Empty observations retain no run payload state. These FMT
operations do not add a GPU backend.

All current built-in `02-format-color` source families have Result CPU operations or helpers that lower to registered operations. FMT-01, FMT-02/03, FMT-04/05A alpha, FMT-06 numeric conversion, FMT-08, FMT-09 transfer, FMT-10 RGB basis, literal-like fill and scalar literal use Result operation ABI 3. The public headers `photospider/ops/format/channel.hpp`, `channel_assembly.hpp`, `channel_editing.hpp`, `alpha.hpp`, `metadata.hpp`, `transfer.hpp` and `rgb_basis.hpp` and `model_conversion.hpp` are installed individually as part of the header-only `Photospider::ops_headers` target; consumers link it together with `Photospider::kernel`. FMT-05B/C are executable compositions of existing Result operations; FMT-04/05A and FMT-09/10 have native keys. Other production operations may remain registered while their structural-image Value contracts remain outside the Result image path. Registry presence alone does not establish Result image support.

## Result image contract

A typed image slot carries a descriptor for one frame/layer, a planar physical layout, semantic facets, and a bounded number of frame/layer samples. Its logical coordinates are `{frame, layer, descriptor axes...}`; frame and layer are separate axes, not color channels. A Result can mix typed image slots and primitive fields under one schema. Do not encode image bytes as `ResultFieldSpec` records.

The compiler resolves the selected named output and its input projection. The Result continuation requests only declared inputs; a callback can read Control samples and then request the Data image support selected by those values. The published relation records both. Dirty transpose uses that same relation, so consumed controls can select new support without treating unrelated inputs as dependencies. `Exact`, `Conservative`, and `Unknown` keep their declared proof meaning.

The Result owner retains schema, certified descriptor facts, backing, relations, and consumed input owners. Admission uses the execution root for work, stages, I/O, relation/maps, payload, and owners. A failed publication is sticky. Cancellation and stale execution retire active callbacks before releasing their phase state. See [Structured Results and image slots](Global-Results.md), [Dependency Data](Dependency-Data.md), and [Tensor Storage and Region Access](../kernel-specs/Tensor-Storage-and-Region-Access.md).

Ordinary numeric Values and non-image structured Results remain available.

## FMT-11 model conversion

FMT-11 A-R and T register 57 CPU keys across strict, Apple Silicon and x86-64 profiles. The installed authoring helpers append one key and return a `values` output containing one tensor; S lowers to `mask.threshold_channel_<profile>` and adds no color key. Inputs contain one Float32 or Float64 tensor and no fields. Full sample rank is at most 8; sample counts follow Result schema representability and execution resource limits. Axes are cell-relative and exclude the batch prefix.

Selected semantic samples request Data and Validation; raw T validates its two-level selector; raw S and bypass samples request Data; constant R output needs only Descriptor. Empty requests are stateless. Q supports generic and spatial view when the full requested mapping has a valid physical view. Other members materialize and reject forced view, including bypass-only requests. Outputs retain schema id, tensor key and batches while updating the selected model description and setting `atomic_trailing_axes` to zero. These are generic tensor operations, not typed image-slot operations.
