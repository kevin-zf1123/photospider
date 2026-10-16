# Channel and color operations

[Chinese reader version](zh/Channel-and-Color-Operations.zh.md).

Bundled format operations use Result ports and the current Result C ABI 5. A Result carries structure and samples, with no attached color, alpha, transfer, sampling or encoding description. Every operation receives the interpretation it needs as explicit static parameters. An explicit description is configuration, not proof of sample validity. The kernel does not reject a connection solely because callers assign different meanings to structurally compatible data.

Public workflow helpers and runtime operations belong to `Photospider::ops`. Consumers link that target, which depends on `Photospider::ops_sdk`, `Photospider::ops_headers` and the kernel. Shared descriptions, codecs, resource wrappers, exact domain mathematics and `format::edit_description` belong to `Photospider::ops_sdk`. The SDK uses only kernel public interfaces. The `TensorDescription` codec selects canonical TDM4 or TDM5 according to its coordinate records; see [Tensor descriptions](Tensor-Semantic-Metadata.md).

## Registered families and configuration

| Family | Public keys or helper | Explicit configuration and output |
| --- | --- | --- |
| [FMT-01](../built-in_ops/02-format-color/op_specs/FMT-01_channel_extraction_contract.md) | `channel.extract_index_<profile>`, `channel.extract_named_<profile>`; `format::split_channels` | `axis`, index or name/role selector, `keepdims`, `layout`; optional `input_description` supplies an axis and named selectors. Output `values` copies bits; split returns individual handles. |
| [FMT-02](../built-in_ops/02-format-color/op_specs/FMT-02_channel_assembly_contract.md) | `channel.assemble_<profile>`, `channel.concatenate_<profile>`, `channel.assemble_mapped_<profile>` | Structural axes/maps and optional per-input `input_descriptions`; output `values` carries no description. |
| [FMT-03](../built-in_ops/02-format-color/op_specs/FMT-03_channel_editing_contract.md) | `format::swizzle_channels`, `format::replace_channels` | Transactional compositions over mapped assembly. Typed literals add `channel.scalar_literal_<profile>` nodes. Both return `values`. |
| [FMT-04](../built-in_ops/02-format-color/op_specs/FMT-04_alpha_association_contract.md) | `alpha.associate_<profile>`, `alpha.unassociate_<profile>` | Optional `input_description` plus `group`; otherwise explicit raw structure/components/alpha source. Output `values`; always materialized, no layout parameter. |
| [FMT-05](../built-in_ops/02-format-color/op_specs/FMT-05_alpha_editing_contract.md) | `alpha.set_<profile>`; `format::extract_alpha`, `format::remove_alpha` | Required explicit `input_description` and group. Set/extract return `values`; removal returns mapped `values` or axis-free `core.identity.value`. |
| [FMT-06](../built-in_ops/02-format-color/op_specs/FMT-06_numeric_conversion_contract.md) | `numeric.convert_format_strict` | `dtype`, `rescale`, explicit/default ranges, conditional `axis`, rounding, overflow and layout. No description parameter. |
| [FMT-08](../built-in_ops/02-format-color/op_specs/FMT-08_metadata_assignment_contract.md) | SDK `format::edit_description` | Pure configuration editing. No workflow key, graph mutation, payload read or resource lookup. |
| [FMT-09](../built-in_ops/02-format-color/op_specs/FMT-09_transfer_contract.md) | `color.transfer_decode_<profile>`, `color.transfer_encode_<profile>` | Explicit curve parameters and optional input description; output `values`. |
| [FMT-10](../built-in_ops/02-format-color/op_specs/FMT-10_rgb_basis_contract.md) | `color.rgb_to_xyz_<profile>`, `color.xyz_to_rgb_<profile>`, `color.adapt_xyz_white_<profile>`; `format::convert_linear_rgb` | Explicit source interpretation and geometry; D generates A→optional C→B with explicit intermediate node parameters. |
| [FMT-11](../built-in_ops/02-format-color/op_specs/FMT-11_model_conversion_contract.md) | 19 native model members across three profiles; S helper uses `mask.threshold_channel_<profile>` | Explicit description or ordered raw selectors and model parameters. Output `values`; Q/R change channel counts. |

The three CPU profiles are `strict`, `accelerated_apple_silicon` and `accelerated_x86_64`; accelerated profiles require matching host capabilities. Numeric conversion has only its strict key. Proposed specification status remains separate from registration status. These families provide no GPU implementation.

## Structural channel operations

Extraction, assembly and editing preserve UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 sample bits, including NaN payloads and signed zero. They do no color conversion, alpha normalization or sample-domain scan. All axis indices are relative to cell axes, excluding the Result batch prefix. Full sample rank is at most eight. Assembly accepts 1..1024 single-tensor Results with no fields and matching nonscalar batch prefixes/nonchannel shapes; its input/output sample limit is 2^40. Extraction instead follows Result backing and requested-region representability, without a family-wide dense product limit.

Extraction requests ordinary member tiles intersecting its selected source coordinates. Arithmetic reads the selected samples; tile authorization may cover additional samples. `keepdims=false` removes the selected cell axis and cannot create rank zero; atomic trailing-axis structure is adjusted when an axis is removed. The split helper asserts canonical source structure and physical layout, including forward producer references. It does not project a description onto outputs.

Assembly inserts, concatenates or maps channel indices. Every connected input participates in static Descriptor checks; payload reads follow only selected coordinates. Mapped records contain input, match, selector and destination, with every destination filled exactly once. There is no output_description or destination_component parameter. Replacement reads original immutable inputs simultaneously; an overwritten base value is not read unless another requested destination selects it. Scalar records explicitly repeat unbatched shape `[1]` values; an ordinary plane is not implicitly broadcast.

Non-reshape views validate each retained backing fragment and can preserve different owners and strides. Reshape requires a compatible affine join. Equal bytes or shared ancestry alone do not prove a valid mapping. Auto materializes only for unavailable physical views, preserving other failures. Materialization publishes requested coverage transactionally under Root budgets and cancellation. Missing samples, padding and unrequested channels are not read. Empty observations perform static checks without sample work.

`channel.literal_like_<profile>` takes one tensor Result and requests only Descriptor support. Prepared `bits`, shape/axis/keepdims, source assertions and layout define the generated output; no output description is accepted. `channel.scalar_literal_<profile>` is a no-input complete-input primitive producing a same-dtype `[1]` tensor from exact native bits.

## Alpha

FMT-04 described association requires an explicit RGB/Gray group with internal alpha and the appropriate source association. It checks the requested finite color and alpha domain even at zero alpha; formulas, rounding, underflow and raw IEEE behavior remain in the family specification. Without a description, raw calls use explicit components and internal, plane or scalar weights. FMT-04 never implicitly inserts alpha and never exports an alpha relationship in metadata.

FMT-05 requires a straight interpretation supplied by the caller. Set uses Float32/Float64 and validates requested new alpha in [0,1] plus requested color. Extract/remove preserve all seven copy dtypes without a new sample-domain scan. Existing alpha extraction copies exact bits; `alpha_encoding` only configures the missing-alpha opaque fallback, whose coverage-one code must be exactly representable.

Channel-bearing removal uses mapped assembly, structural assertions and its layout policy. Axis-free missing-alpha identity uses `core.identity`, returns `value`, adds no source assertion and performs no layout conversion. A downstream consumer must receive its own description. Read alpha does not become published alpha coverage; later consumers request needed samples normally.

## Numeric format and transfer

FMT-06 implements all 49 supported dtype pairs. Rescale defaults to true with independent dtype-derived source and target intervals; pure cast rejects ranges and axis. Per-channel range tables use explicit cell axes. The exact affine result rounds once to the destination, then applies reject/clip. The Result retains structural schema identity, tensor key, shape and batches, changes dtype and sets `atomic_trailing_axes=0`. No decoder is propagated. Ordinary payload tile Needs provide the required coverage; a same-dtype static identity can retain a legal view. SIMD/SME paths share the strict numerical contract and require separate native-dispatch evidence when measured.

FMT-09 with `input_description` requires a unique group and forbids raw axis/components. Decode can resolve its full curve from that configuration; encode requires a target curve and a linear source interpretation. Without a description, explicit components and curve are required; index lists require axis. Curve-specific units/reference checks are retained. Output keeps shape/dtype and sets `atomic_trailing_axes=0`, with no transfer label.

Linear and gamma=1 are bit-copy identities. A legal complete identity may use a view while retaining required described-domain validation. Other mappings materialize; forced view rejects a statically nonidentity transform. Simple paths process at most 1024 samples per run, general curves at most 64 per batch, with lazy workspace admission and floating-environment restoration.

## Basis, white and model conversions

FMT-10 applies exact RGB/XYZ matrices and explicitly selected XYZ Scaling, Bradford, CAT02 or CAT16 adaptation. Described calls require a selected RGB/XYZ group; raw calls require axis, ordered components and geometry. Nonidentity rows consume all three source terms, including zero coefficients; exact identity reads only the corresponding sample and still validates it when configured. Bypass alpha/AOVs are exact copies. Transfer, exposure, gamut and rendering remain separate operations.

D stages its entire graph expansion before mutation. It derives intermediate XYZ configurations from the supplied description and writes them into generated node parameters. It does not recover descriptions from graph edges. A→B and A→C→B retain each node's rounding and failures; same source/target basis does not authorize replacing the composition with identity.

FMT-11 A–R/T supply 57 profile-specific native model keys; S adds three MASK threshold keys. An explicit description can resolve a unique group and permits matching axis/components assertions. Raw calls provide the member's selectors, white, hue unit, NCL coefficients, Gray kind or constants. Raw XYZ→xyY requires white_x/white_y as declared by the implementation. Q removes two selected slots; R expands a Gray slot or inserts an explicit output axis. No output model description is attached.

Only Q supports forced view. Selected samples use ordinary payload tile Needs, with described-domain checks performed by the operation. Raw T always validates its exact 0/1 selector. Raw S and bypass read their selected samples; constant R uses explicit descriptor-only access. Publication, errors, owners, budgets and cancellation follow the Result contract. Numerical reports use the SDK `report_numeric`/reader over the kernel's bounded generic diagnostic channel; cache hits do not report unperformed arithmetic.
