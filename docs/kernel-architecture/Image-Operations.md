# Image operations

## Scope and ownership

Images use ordinary Result tensor slots with `layout.spatial = true`. The kernel owns tensor structure, authorized coverage, backing and execution. It does not infer RGB, alpha, a color model or numeric range from a channel count. Each consuming operation receives the description, geometry, transfer and sampling parameters it needs explicitly.

Bundled image and format operations are compiled into `Photospider::ops`. That target exposes the authoring headers and depends on `Photospider::ops_sdk` and the kernel. Shared descriptions, codecs, ICC/OCIO resource wrappers and pure description editing belong to `Photospider::ops_sdk`. An SDK-only consumer does not need the operation registry. See [Channel and color operations](Channel-and-Color-Operations.md) and its linked family specifications for exact parameters and formulas.

## Production families

| Family | Production implementation and public interface | Current behavior |
| --- | --- | --- |
| Channel extraction | `channel_extraction.cpp`; `photospider/ops/format/channel.hpp` | Six profile-specific index/name Result operations. Explicit descriptions may resolve axes and selectors; samples are copied exactly. `split_channels` expands index nodes. |
| Channel assembly and editing | `channel_assembly.cpp`; `channel_assembly.hpp`, `channel_editing.hpp` | Assembly, concatenation and mapped assembly copy seven supported dtypes. Swizzle/replace are transactional authoring helpers over those keys and scalar literals. |
| Literal-like fill | `channel_literal_like.cpp` | Requests only Descriptor support and repeats prepared same-dtype bits. It supplies the opaque fallback for alpha extraction. |
| Scalar literal | `channel_assembly.cpp` | No-input complete-input primitive producing shape `[1]`, used by explicit scalar/literal channel sources. |
| Alpha | `alpha_operations.cpp`, `alpha_common.hpp`, `alpha_authoring.cpp`; `alpha.hpp` | Nine native keys cover associate, unassociate and set across three CPU profiles. Extract/remove are authoring compositions. |
| Numeric conversion | `numeric_conversion.cpp` | `numeric.convert_format_strict` implements 49 pairs among seven dtypes using explicit numeric ranges. |
| Transfer | `transfer.cpp`; SDK `photospider/ops/format/transfer.hpp` | Encode/decode keys use explicit transfer definitions and selected RGB/Gray groups or raw components. The SDK header provides the definition codec. |
| RGB basis and white | `rgb_basis.cpp`; `rgb_basis.hpp`, SDK `rgb_geometry.hpp` | A/B/C keys perform RGB/XYZ and explicit white adaptation. The D helper composes those nodes. |
| Models | `model_conversion.cpp`, `model_math.cpp`, `model_simd.cpp`; `model_conversion.hpp` | A–R/T provide 57 profile-specific CPU keys; S lowers to the MASK threshold key. |
| Description editing | SDK `common/domain/description_editor.cpp`; `photospider/ops/format/metadata.hpp` | `format::edit_description` returns a configuration value. It creates no graph node; `metadata.assign/remove` are not registered. |
| LUT bake and application | `plugins/ops/01-numeric/`; `photospider/ops/numeric/lut3d_bake.hpp` | Ordinary numeric Results and explicit configuration. Report/table schemas and validation belong to the SDK, not the kernel's spatial-storage machinery. |

The current Result operation C ABI is 4. The CPU profiles are `strict`, `accelerated_apple_silicon` and `accelerated_x86_64`; accelerated profiles require the matching host capability. Registration does not imply support for every spatial layout or a GPU backend.

## Alpha and model operations

Native alpha operations accept one tensor , preserve batch prefixes, and interpret axis parameters relative to cell axes. Complete sample rank is at most 8 and sample count at most 2^40. Association arithmetic uses same-dtype Float32/Float64; extraction, removal and component copies retain the seven supported raw-bit dtypes.

FMT-04 associate/unassociate always materialize and expose no layout parameter. FMT-05A Set accepts a layout policy. A generic Set view must prove one `CpuStorage` owner and one affine mapping; spatial Set views are limited to internal identity. Auto materializes only for `ViewUnavailable`. Publication is transactional under Root budgets and cancellation; Empty observations retain no run payload state. These operations add no GPU backend.

`extract_alpha` lowers an existing channel to extraction and an opaque fallback to literal-like fill. `remove_alpha` lowers channel-bearing input to mapped assembly and returns `values`; its axis-free identity branch uses `core.identity` and returns `value`. Neither composition attaches a description to its output. The caller supplies the resulting interpretation to later consumers.

FMT-11 inputs contain one Float32/Float64 tensor. Complete sample rank is at most 8; sample counts follow schema representability and resource limits. Selected samples are acquired through ordinary payload tile Needs and checked according to explicit operation configuration. Raw T validates its two-level selector; raw S and bypass read selected samples; constant R uses explicit descriptor-only access. Empty requests are stateless. Q can retain a representable generic or spatial view; other members materialize and reject forced view, including bypass-only requests. Outputs retain structural schema identity, tensor key and batch axes and set `atomic_trailing_axes` to zero. Their intended output interpretation is defined by the operation and its explicit parameters, never attached to the Result.

## Spatial Result contract

A generic spatial Tensor has explicit cell-relative H/W axes and optional channel metadata, with complete sample rank at most eight including its batch prefix. H/W must be distinct and outside atomic trailing axes; other cell axes are permitted and remain fully covered by spatial tiles. Particular image operations may require rank-two or rank-three cells, as their own contracts specify. Logical axis order is independent of physical layout. Kernel schemas have no component-group field, and atomic observation closure is distinct from tile geometry.

Every Result member is a static Tensor with its own grid. Control values use ordinary payload Needs before they can select later input tiles. The kernel records actual tile dependencies and frozen shared evidence without relation roles or guarantee levels. Spatial tiles cover every non-H/W cell axis; atomic closure does not reduce that coverage.

A Result owner retains schema, certified descriptor facts, backing, ObjectId association and any source owners required by a view. Operation resource configurations remain in preparation, plan and execution state. Root admission covers work, stages, I/O, tile evidence and geometry metadata, payload and owners. Publication failures are sticky; cancellation and stale execution retire admitted callbacks before releasing their borrowed phase state.

See [Structured Results](Global-Results.md), [Dependency Data](Dependency-Data.md), and [Tensor Storage and Region Access](../kernel-specs/Tensor-Storage-and-Region-Access.md).
