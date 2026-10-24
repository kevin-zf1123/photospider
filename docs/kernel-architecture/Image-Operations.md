# Image operations

## Scope and ownership

Images use ordinary Result tensor slots with `layout.spatial = true`. The kernel owns tensor structure, authorized coverage, backing and execution. It does not infer RGB, alpha, a color model or numeric range from a channel count. Each consuming operation receives the description, geometry, transfer and sampling parameters it needs explicitly.

Bundled image and format operations are compiled into `Photospider::ops`. That target exposes the authoring headers and depends on `Photospider::ops_sdk` and the kernel. Shared descriptions, codecs, ICC/OCIO resource wrappers and pure description editing belong to `Photospider::ops_sdk`. An SDK-only consumer does not need the operation registry. See [Channel and color operations](Channel-and-Color-Operations.md) and its linked family specifications for exact parameters and formulas.

## Production families

| Family | Production implementation and public interface | Current behavior |
| --- | --- | --- |
| Channel extraction | `channel_extraction.cpp`; `photospider/ops/format/channel.hpp` | Six fixed ABI 8 index/name CPU operations copy selected samples to independent dense tiles with zero scratch. Nonempty view fails; split expands index nodes. |
| Channel assembly and editing | `channel_assembly.cpp`; `channel_assembly.hpp`, `channel_editing.hpp` | Fixed ABI 8 assembly, concatenation and mapped tasks copy seven dtypes to independent dense tiles with zero scratch. Swizzle/replace compose those keys and scalar literals. |
| Literal-like fill | `channel_literal_like.cpp` | Fixed ABI 8 CPU tasks retain only input schema dependencies and fill selected output tiles with prepared same-dtype bits and zero scratch. Supplies the opaque fallback for alpha extraction. |
| Scalar literal | `channel_assembly.cpp` | Zero-input, zero-scratch fixed ABI 8 task producing one exact shape-[1] tile for explicit scalar/literal sources. |
| Alpha | `alpha_operations.cpp`, `alpha_common.hpp`, `alpha_authoring.cpp`; `alpha.hpp` | Nine fixed ABI 8 keys cover associate, unassociate and set across three CPU profiles. Extract/remove are authoring compositions. |
| Numeric conversion | `numeric_conversion.cpp` | `numeric.convert_format_strict` uses fixed ABI 8 for all 49 pairs among seven dtypes, with explicit ranges, fixed exact scratch and independent dense output tiles. |
| Transfer | `transfer.cpp`; SDK `photospider/ops/format/transfer.hpp` | Six fixed ABI 8 CPU encode/decode keys use explicit transfer definitions and selected RGB/Gray groups or raw components, source tile closure, declared numerical scratch and independent outputs. Semantic sample validity is a caller precondition. The SDK provides the definition codec. |
| RGB basis and white | `rgb_basis.cpp`; `rgb_basis.hpp`, SDK `rgb_geometry.hpp` | Nine A/B/C fixed ABI 8 CPU keys perform RGB/XYZ and explicit white adaptation using static source tile closure, fixed scratch and independent dense outputs. Semantic sample validity is a caller precondition. D preserves constituent rounding. |
| Models | `model_conversion.cpp`, `model_math.cpp`, `model_simd.cpp`; `model_conversion.hpp` | A–R/T provide 57 fixed ABI 8 CPU keys; S lowers to three fixed MASK threshold keys. |
| Gamut and luminance mapping | `gamut_tone.cpp`, `color_point.hpp` | Six strict fixed ABI 8 CPU keys for FMT-14 A-D and FMT-15 A/B; Float32/64, exact component demands, finite-domain failures, independent outputs and fixed arithmetic scratch. |
| ICC, OCIO and proofing | icc_operations.cpp, ocio_operations.cpp; format/external_color.hpp | Eleven fixed ABI 8 CPU keys for FMT-12 A-C, FMT-13 A-F and FMT-18 A/B; private pinned engines, Float32/64, closed immutable resources and exact color/bypass demand. |
| Explicit view | `view_authoring.cpp`; `photospider/ops/format/view.hpp` | `format::compose_view` transactionally composes native FMT-09/10/14/15 and configured FMT-13A..F stages; resource-aware overloads validate actual processors and native/configured final display assertions. |
| Description editing | SDK `common/domain/description_editor.cpp`; `photospider/ops/format/metadata.hpp` | `format::edit_description` returns a configuration value. It creates no graph node; `metadata.assign/remove` are not registered. |
| Masks and morphology | `plugins/ops/04-mask-morphology/`; SDK `photospider/ops/mask.hpp`, `photospider/ops/mask/gap_fill.hpp` | MASK-01..17 contain 61 strict CPU fixed ABI 8 primitives and two public authoring workflows. |
| LUT bake and application | `plugins/ops/01-numeric/`; `photospider/ops/numeric/lut3d_bake.hpp` | Ordinary numeric Results and explicit configuration. Report/table schemas and validation belong to the SDK, not the kernel's spatial-storage machinery. |

See [Channel and color operations](Channel-and-Color-Operations.md) for each family's invocation protocol. Native numerical CPU profiles are `strict`, `accelerated_apple_silicon` and `accelerated_x86_64`; accelerated profiles require the matching host capability. Registration does not imply support for every spatial layout or a GPU backend.

External-engine construction is synchronous and noncancellable, with memory/work admission and complete Empty/bypass validation. Pixel execution remains cancellable. No frozen kernel change is required. See [implementation, independent validation and performance](../built-in_ops/02-format-color/FMT-12-13-18_implementation.md).

## Alpha and model operations

Native alpha operations accept one tensor , preserve batch prefixes, and interpret axis parameters relative to cell axes. Complete sample rank is at most 8 and sample count at most 2^40. Association arithmetic uses same-dtype Float32/Float64; extraction, removal and component copies retain the seven supported raw-bit dtypes.

FMT-04 associate/unassociate use fixed input tile unions and independent dense outputs, with no layout parameter. Semantic numerical validity is a caller premise; raw IEEE results remain defined. Copy-only tasks declare zero scratch, arithmetic tasks 4096 bytes, and reference arithmetic 6888 bytes. FMT-05A Set auto/materialize copy independent outputs with zero scratch. Nonempty view has schema-only dependencies and fails ViewUnavailable; Empty has no tasks. Publication follows successful ordered call retirement. These operations add no GPU backend.

`extract_alpha` lowers an existing channel to extraction and an opaque fallback to literal-like fill. `remove_alpha` lowers channel-bearing input to mapped assembly and returns `values`; its axis-free identity branch uses `core.identity` and returns `value`. Neither composition attaches a description to its output. The caller supplies the resulting interpretation to later consumers.

FMT-11 inputs contain one Float32/Float64 tensor. Complete sample rank is at most 8; sample counts follow schema representability and resource limits. Each selected output tile has a fixed task whose source reads close its component mappings on the input grid. Semantic numerical validity and T's binary selector are caller preconditions. Constant R tasks without other source-dependent outputs use schema-only access and zero scratch; pure copies also need zero scratch. Auto/materialize create independent dense outputs. Q's nonempty forced view task is schema-only and fails ViewUnavailable; other members reject forced view statically. Empty creates no payload task. Outputs retain structural schema identity, tensor key and batch axes and set atomic_trailing_axes to zero. Their intended interpretation is explicit configuration and is not attached to the Result.

## Spatial Result contract

A generic spatial Tensor has explicit cell-relative H/W axes and optional channel metadata, with complete sample rank at most eight including its batch prefix. H/W must be distinct and outside atomic trailing axes; other cell axes are permitted and remain fully covered by spatial tiles. Particular image operations may require rank-two or rank-three cells, as their own contracts specify. Logical axis order is independent of physical layout. Kernel schemas have no component-group field, and atomic observation closure is distinct from tile geometry.

Each Result member has its own static tile grid. Fixed operations declare input closure, COPYs and task dependencies during compilation; runtime schedules from completions and retires calls in order. Every produced tile has independent preallocated storage, while concrete source strides, offsets and capacities are validated before execution. Spatial metadata describes axis topology separately from those physical views.

Extraction, assembly, literals and their related helpers use the fixed ABI 8 paths listed here. Other families retain their individually documented invocation/storage protocols. Result owners retain actual storage. On cancellation or failure, the fixed executor drains submitted tasks before releasing borrowed inputs and task state.

See [Structured Results](Global-Results.md), [Dependency Data](Dependency-Data.md), and [Tensor Storage and Region Access](../kernel-specs/Tensor-Storage-and-Region-Access.md).
