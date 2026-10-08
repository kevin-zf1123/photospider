# Channel and color operations

The default registry exposes FMT-01 extraction, FMT-02 assembly, FMT-03 editing authoring helpers over assembly, FMT-04 alpha association, FMT-05A alpha setting, FMT-06 numeric conversion, FMT-08 metadata assignment, FMT-09 transfer conversion, FMT-10 RGB basis conversion, the `channel.literal_like_<profile>` and `channel.scalar_literal_<profile>` Result primitives, and FMT-11 model conversions. FMT-05B/C remain compile-time helper compositions over registered operations; FMT-10D is a public graph helper over registered A/B/C operations. The twelve FMT-02/FMT-03-related CPU keys and sixty FMT-11-related keys use Result operation ABI 2 and single-tensor Results. See the [Chinese mirror](zh/Channel-and-Color-Operations.zh.md) for the same current contract.

The table distinguishes registered Result operations from remaining Proposed specifications. A Proposed decision status does not change registration status.

## Current Result families and retained FMT contracts

| Family | Keys and public authoring API | Current status |
| --- | --- | --- |
| Channel selection | `channel.extract_index_<profile>`, `channel.extract_named_<profile>`; `format::split_channels` | Registered Result operations; one tensor member and no fields; exact selected-channel Data plus Descriptor support. |
| Literal-like fill | `channel.literal_like_<profile>` | Registered one-input Result operation; reads Descriptor support only and repeats prepared bits according to the output descriptor. |
| Scalar literal | `channel.scalar_literal_<profile>` | Registered no-input Whole Result primitive for prepared native bits in shape `[1]`; used by FMT-03 scalar sources. |
| Metadata editing | `metadata.assign_<profile>`; `format::assign_metadata`, `format::remove_metadata` | Registered Result operations; one tensor member and no fields; same-coordinate Data plus Descriptor roles, without Validation/Control or planar callbacks. |
| Channel assembly and editing | `channel.assemble_<profile>`, `channel.concatenate_<profile>`, `channel.assemble_mapped_<profile>`; installed helpers in `channel_assembly.hpp` and `channel_editing.hpp` | Registered Result keys; FMT-03 helpers lower transactionally to mapped assembly. There is no native swizzle/replace key. |
| Alpha association and editing | `alpha.associate_<profile>`, `alpha.unassociate_<profile>`, `alpha.set_<profile>`; installed helpers in `photospider/format/alpha.hpp` | Nine Result ABI 2 keys cover three members across the strict, Apple Silicon and x86-64 CPU profiles. FMT-05B/C helpers compile transactionally into registered extraction, literal-like fill, mapped assembly and metadata assignment operations. |
| Numeric format conversion | `numeric.convert_format_strict` | One registered strict Result ABI 2 key for all 49 pairs among seven supported dtypes. |
| Transfer conversion | `color.transfer_encode_<profile>`, `color.transfer_decode_<profile>`; `TransferDefinition` codec in `photospider/format/transfer.hpp` | Six registered Result ABI 2 keys across encode/decode and three CPU profiles; generic and spatial single-tensor inputs. |
| RGB basis conversion | `color.rgb_to_xyz_<profile>`, `color.xyz_to_rgb_<profile>`, `color.adapt_xyz_white_<profile>`; installed helpers in `photospider/format/rgb_basis.hpp` | Nine registered Result ABI 2 CPU keys cover A/B/C across three profiles. `format::convert_linear_rgb` transactionally composes registered stages; it adds no D key. |
| Model conversion | `color.*_<profile>` for A-R and T; FMT-11S helper lowers to `mask.threshold_channel_<profile>` | 57 native model keys across three CPU profiles; S adds no color key. One-tensor Float32/Float64 Results, rank at most 8; sample counts follow Result schema representability and execution resource limits. |

Profile-suffixed channel extraction, assembly, alpha, metadata, transfer, RGB basis, literal-like fill and scalar-literal operations use `strict`, `accelerated_apple_silicon` or `accelerated_x86_64`; accelerated variants require the matching host capability. Numeric conversion uses its single unsuffixed strict key. Model-conversion keys use the strict, accelerated Apple Silicon and accelerated x86-64 profiles. Current Result operations use `start_result` and the Result tensor protocol.

The public tensor-description encoder emits v4 when no `coordinates` record is present and v5 when a tensor, component, channel or group carries `coordinates`. The decoder accepts matching v4/TDM4 and v5/TDM5 encodings; older versions and discriminator mismatches reject. Compatible coordinate assertions merge field by field, and unequal nonempty assertions for the same field fail. See [Tensor Semantic Metadata](Tensor-Semantic-Metadata.md) for the full schema and [FMT-11 model conversion contract](../built-in_ops/02-format-color/op_specs/FMT-11_model_conversion_contract.md) for model-specific requirements.

## FMT-01 channel extraction

`channel.extract_index_{strict,accelerated_apple_silicon,accelerated_x86_64}` selects static `index`. `channel.extract_named_` with the same profile suffixes selects one exact `selector` in the separate `match=name|role` namespace. The named CPU profile must match the host. All six Result keys preserve UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64 bytes, including NaN payloads.

Each node accepts one Result with a single tensor member and no fields, and publishes one `values` Result. Parameters `metadata_mode=respect|raw|override`, `axis`, `keepdims`, and `layout=auto|view|materialize` are static. `axis` indexes the tensor descriptor, excluding the Result batch prefix; execution maps it after that prefix. Respect checks the axis against the TensorDescription. Override uses a node-local TensorDescription encoded by `tensor_description_parameter`. Named selection requires a complete channel table and cannot use raw mode. The input/output preserve batch axes; `keepdims=false` removes the selected cell axis and requires descriptor rank at least two. No color conversion, alpha normalization, sample validation or floating arithmetic occurs.

The public authoring interface is installed from `photospider/format/channel.hpp`. The runtime projects the selected TensorDescription component and remaps applicable cell-axis metadata. The normal extraction output retains the projected TensorDescription facet, not every opaque input annotation. A projected component does not acquire complete-color or sample-validity guarantees. `format::split_channels` expands one index node per channel and returns `c0`, `c1`, ... handles after checking the declared schema and physical layout. Each generated node carries a bounded, domain-separated SHA-256 digest of the complete canonical schema plus a separate physical-layout assertion for compile-time producer checks. The 64-character digest includes opaque metadata and batch axes; it hashes no samples and is not a validity proof.

Dependency-v2 requests exact selected-channel source coverage and its Descriptor with Data (1) and Descriptor (8) roles. It requests no Validation or Control support. Generic tensor views support valid positive, negative and zero strides, and Result backing partitions can retain multiple owners. Selecting a spatial tensor's channel axis can retain the source plane owner. Slicing spatial height or width materializes in auto mode; forced view returns `InvalidArgument/InvalidDomain` with `ViewUnavailable`. Materialization copies only requested output coverage, checking cancellation within runs of at most 256 samples. Empty output demand publishes an empty Result without reading source payload. Result caching is disabled.

The [public Result workflow](../../examples/channel_extraction_workflow/README.md) describes a B/A/R/G UInt8 tensor of shape `[2,2,4]`, requests the second row of `c2`, and checks `[12,13]`. `test_channel_extraction` covers direct index and named operations, arbitrary axes, all seven dtypes, batch/spatial layouts, owner partitions, metadata, resources, cancellation and limits.

```sh
cmake --build build --target test_channel_extraction -j 8
ctest --test-dir build -R '^test_channel_extraction$' --output-on-failure
```

The current Result integration test and public workflow pass on the native CPU host. This does not establish support for another ISA or a GPU. The older [channel extraction performance workflow](../../examples/channel_extraction_performance/README.md) contains Value/planar measurements. It is not timing evidence for the Result implementation.


## Scalar literal primitive

The default registry registers `channel.scalar_literal_<profile>` for the
three CPU profiles. This no-input Whole Result operation publishes a generic
shape `[1]` tensor with the selected dtype and prepared lowercase hexadecimal
native bytes. It supports UInt8, UInt16, Int8, Int16, Int64, Float32 and
Float64. It is used by FMT-03 literal sources and does not implement an alpha
helper. Empty output demand has no retained payload state; result state is
local to each run.

## Literal-like fill primitive

The default registry registers `channel.literal_like_<profile>` for the three CPU profiles. Each operation accepts one Result containing one tensor member and publishes one Result with one tensor member. Execution requests Descriptor support only (role 8); it reads no source sample payload. The operation repeats same-dtype raw bits from `bits` over the requested output coordinates, using `output_description`, optional cell-axis `axis`, and `keepdims` to define the tensor. It retains batch axes, schema id, tensor key, opaque facets and owned resources. The full sample count is in `[1, 2^40]`. Its optional `axis` indexes cell axes
and excludes the Result batch prefix.

Required static parameters are `bits`, `expected_inputs`, `output_description`, `layout`, `authoring_member`, and `keepdims`; `axis` is optional. The `expected_inputs` assertion uses `result-v1`, a 64-character schema digest, and a physical-layout assertion. `auto` and `materialize` generate requested bytes; a nonempty forced `view` returns `ViewUnavailable`. Empty observations are stateless. `test_channel_literal_like` and its installed consumer pass on the native CPU host. This primitive can serve the opaque FMT-05B lowering path, but it is not the public `extract_alpha` helper.

## FMT-02 channel assembly

`channel.assemble_<profile>`, `channel.concatenate_<profile>` and
`channel.assemble_mapped_<profile>` each have strict, Apple Silicon and x86
CPU profiles. Each operation accepts 1 to 1024 input Results, each with one
tensor member and no fields, and publishes one `values` Result. The seven
supported dtypes are copied bit-for-bit. Inputs share an exact batch prefix;
nonscalar source sample shapes must agree outside the channel axis. Complete
input and output sample counts are at most 2^40, and the full sample rank,
including batch and cell axes, is at most 8. Member-specific `axis`, `input_axes`,
and `output_axis` parameters index cell axes and exclude the batch prefix. Output
retains the first source schema id, tensor key, global metadata and publication policy. It rebuilds the
TDM tensor facets and does not propagate unknown tensor facets.

A inserts a channel axis into equal component shapes. B joins ordered channel
blocks and permits a distinct source axis per input. C maps explicit component
or channel selectors to every output slot exactly once; connected but unused
inputs still receive Descriptor checks. All source payload requests are limited
to mapped coordinates in the requested output regions. There is no broadcasting,
resampling, conversion or sample-domain validation.

The installed helpers in `photospider/format/channel_assembly.hpp` append static
graph nodes and perform no sample reads. `metadata_mode`, `layout`, profile,
axis and map records are serialized through the current parameter interface.
The compiler/runtime enforce input descriptors and resource capacity.

A generic retained view requires the entire requested Q to use one
`CpuStorage` owner and one affine address expression. A planar view additionally
requires ResultBuilder proof of one root, consecutive planes in output order,
and the expected row pitch. A forced view without that proof reports
`ViewUnavailable`; `auto` materializes only for that status. Materialization
reads requested regions only. Each run owns independent prepared state, and
empty observations keep no payload state. Need admission uses groups of 64;
execution processes at most 16 groups plus publication. The 1024-input case
requires explicit 64 MiB metadata capacity; the default 16 MiB capacity returns
a controlled resource failure. The [performance guide](../../examples/channel_assembly_performance/README.md)
records current Result smoke coverage and labels its earlier measurements as
historical Value/planar evidence.

## FMT-03 channel editing

`format::swizzle_channels` and `format::replace_channels` are installed graph
authoring helpers. They expand into `channel.assemble_mapped_<profile>` and
`channel.scalar_literal_<profile>` when literals are used; no separate native
swizzle/replace key exists. Swizzle maps an ordered nonempty output slot list.
Replacement assigns distinct original destinations simultaneously, and an
empty replacement list is identity. Scalar literals preserve explicitly
provided native bits. Connected inputs, including unused inputs, contribute
Descriptor checks; requested Data follows effective source mappings.

FMT-03 requires every nonscalar input to have the same batch prefix as the base; scalar inputs are unbatched shape `[1]`. Its axis parameters index cell axes and exclude the batch prefix. The full sample rank, including batch and cell axes, is at most 8. FMT-03 input assertions use `result-inputs-v1:` plus 64 lowercase SHA-256 hex
digits. The domain-separated digest covers the complete ordered canonical schema
and layout of every connected input, including repeated and unused inputs. It
contains no sample data and proves no sample validity. The literal operation
uses Whole semantics over shape `[1]` with prepared native bits. Scalar row
repetition copies at most 256 samples per batch while charging work and polling
cancellation. Public Result workflow output is documented in [the example](../../examples/channel_editing/README.md).

## FMT-04 alpha association

`format::associate_alpha` and `format::unassociate_alpha` append the registered
`alpha.associate_<profile>` and `alpha.unassociate_<profile>` Result operations.
Each member has strict, Apple Silicon and x86-64 CPU profiles. Inputs use one
single-tensor Result with no fields; optional second Result inputs are reserved
for explicit raw alpha plane or scalar weights. Semantic color and consumed
alpha receive Data and Validation support, pass-through-only values are copied
without extra validation, and all connected inputs receive Descriptor support.
No Control support is used. See the [FMT-04 contract](../built-in_ops/02-format-color/op_specs/FMT-04_alpha_association_contract.md)
for formulas, raw-mode behavior, layout and error rules.

## FMT-05 alpha extraction and removal

The installed `photospider/format/alpha.hpp` exposes `format::extract_alpha` and
`format::remove_alpha` as compile-time graph compositions. They accept a
single-tensor Result with no fields, preserve one of the seven supported dtypes
bit-for-bit, retain batch axes, and interpret axes relative to cell axes. The
complete sample rank, including batch and cell axes, is at most 8; the complete
sample count is at most 2^40. They do not scan samples to validate an alpha
domain.

Extraction with an existing alpha lowers to `channel.extract_index`; the
explicit opaque fallback lowers to `channel.literal_like`, which requests only
Descriptor support. Removal of channel-bearing data lowers to mapped assembly;
component Gray identity uses `metadata.assign` and still requests Data. The
helpers assert source schema and physical layout at compile time. FMT-05A
`set_alpha` and FMT-04 association helpers use the registered native keys
`alpha.set_<profile>`, `alpha.associate_<profile>`, and
`alpha.unassociate_<profile>`. Historical Value/planar alpha measurements are
not Result performance evidence.

## FMT-06 numeric conversion

`numeric.convert_format_strict` is the single registered strict Result ABI 2 key.
It accepts one Result containing one tensor member and no fields, then publishes
one `values` output Result containing one tensor. The seven supported source and
target dtypes provide 49 conversion pairs. Complete sample rank, including batch
and cell axes, is at most 8; sample count is at most 2^40. Output preserves Result
schema identity, tensor key, logical shape and batch axes. It updates tensor dtype
and encoding facets, and sets `atomic_trailing_axes` to zero.

The operation requests source Data (role 1) and Descriptor (role 8). It requests
no Validation or Control support, scans no unrequested samples, and keeps errors
within the requested observation scope. Axis parameters index cell axes and
exclude batch axes. Numerical behavior, typed endpoint formats and metadata
propagation are defined in the [FMT-06 contract](../built-in_ops/02-format-color/op_specs/FMT-06_numeric_conversion_contract.md). The migrated `test_numeric_conversion` suite passes 49 dtype pairs, randomized oracles, ROI, cold-lookup budget, failure-order, floating-environment and same-coordinate identity-view coverage. Additional batch/view/Empty/bit-stride, ICC-resource, concurrent-plan-reuse and payload-release checks pass; `test_numeric_conversion_sme` passes. `test_alpha_numeric_interop` passes four inherited/moved cases; this does not establish model-conversion interoperability. The six focused numeric/alpha/resource CTests pass. The installed numeric-conversion and alpha-numeric-interoperability consumers pass; the standalone performance consumer builds. Thirteen serial Result smoke cases pass byte oracles, but no Result performance conclusion is drawn.

A same-dtype mapping may publish a generic or spatial view only when the complete
declared mapping is a static identity and the source storage can represent it.
A request restricted to one identity channel does not make a transforming whole
tensor viewable. Forced view reports `ViewUnavailable` when proof fails; `auto`
materializes, and materialization publishes requested target-width samples
transactionally under Root budgets and cancellation. Empty demand is stateless.
For a full sample coordinate `q`, the table index is selected at
`q[batch_prefix_length + axis]`; `axis` itself is a cell-axis index that excludes
the Result batch prefix.

The single strict key uses exact scalar conversion plus internal AArch64 NEON,
runtime-checked amd64 AVX2 and eligible Apple SME Float32→UInt8 paths. These paths
share one numerical contract. No GPU implementation is provided. The linked
performance README preserves earlier Value/planar measurements and experiments;
those are not current Result performance evidence. Result performance has no
conclusion here.

## FMT-09 transfer encoding and decoding

The registry provides `color.transfer_decode_<profile>` and
`color.transfer_encode_<profile>` for the strict, accelerated Apple Silicon and
accelerated x86-64 CPU profiles. Each operation accepts one Result containing
one Float32 or Float64 tensor and no fields, and publishes one `values` output
Result containing one tensor. It preserves Result schema identity, tensor key,
logical shape and batch axes, while updating semantic transfer facets and setting
`atomic_trailing_axes` to zero. The complete sample rank, including batch and
cell axes, is at most 8; the sample count is at most 2^40.

Semantic mode selects an explicit RGB or Gray group. Participating color samples
request Data and Validation; pass-through samples request Data only. Raw mode
selects explicit components or all components and requests Data without semantic
domain validation. Every connected input requests Descriptor support; these
operations request no Control. `axis` is a cell-axis index, excluding the Result
batch prefix. Empty output demand is stateless.

Linear and `power_gamma` with gamma 1 are bit-copy identities. A legal complete
identity map can view generic or spatial Result storage, including semantic
requests that still validate the selected samples. Forced view rejects a
statically nonidentity mapping during compile/direct preflight; an identity view
whose physical representation cannot be proven fails during observation
evaluation. Auto materializes only when the operation is nonidentity or a
physical view cannot be represented. Materialization publishes requested
coverage transactionally under Root budgets and cancellation, with normal owner
and resource lifetimes. The numeric workspace is allocated lazily; simple runs
process up to 1024 samples, while the general curve path batches at most 64. Strict
fallback and floating-environment behavior follow NUM. Transfer operations have
no GPU backend. The focused `test_transfer_operations` and
`test_transfer_runtime`, `test_transfer_math` and `test_transfer_simd` pass, as do
six shared FMT regression tests, `test_result_execution` and
`test_shared_results`.
The installed-package consumers `installed_transfer_operations` and
`installed_transfer_runtime` pass 2/2, and the standalone installed-package
performance consumer configures and builds. The integration test completed
16,760 golden attempts on strict and Apple Silicon profiles; x86-64 was skipped
because the backend is unavailable on this host. Twenty-five selected serial
Result performance smoke cases passed their golden gates, with two warmups and
one measured execution each. They do not establish a full matrix or performance
conclusion. Historical Value/planar measurements remain separate; see the
[transfer performance guide](../../examples/transfer_performance/README.md).

## FMT-10 RGB basis and XYZ

The registry provides `color.rgb_to_xyz_<profile>`,
`color.xyz_to_rgb_<profile>` and `color.adapt_xyz_white_<profile>` for the strict,
accelerated Apple Silicon and accelerated x86-64 profiles. Together these are
nine Result ABI 2 CPU keys. Each accepts one Result containing one Float32 or
Float64 tensor and no fields, and publishes one `values` Result with one tensor.
The result preserves Result schema identity, tensor key, logical shape and batch
axes, updates applicable semantic facets and sets `atomic_trailing_axes` to zero.
Complete sample rank including batch and cell axes is at most 8; sample count is
at most 2^40. The installed `photospider/format/rgb_basis.hpp` header exposes
static codecs and `format::rgb_to_xyz`, `format::xyz_to_rgb`,
`format::adapt_xyz_white` and `format::convert_linear_rgb` authoring helpers.

A/B transform RGB and XYZ coordinates using the exact matrices in the FMT-10
contract. C applies explicit full chromatic adaptation with XYZ Scaling,
Bradford, CAT02 or CAT16. D is not a native operation: its helper stages A,
optional C, then B, preserving each stage's rounding and failures. Transfer,
exposure, gamut mapping, alpha association and scene/display conversion remain
separate operations.

Semantic operations select a complete ordered RGB or XYZ group. Nonidentity
requests read all three selected source samples for each requested output row,
including terms whose matrix coefficient is zero. They request Data plus
Validation for those selected samples; exact-identity matrices read only the
corresponding selected sample while semantic mode still validates it. Bypass
components such as alpha and AOVs read their matching Data only. Raw mode reads
the explicit ordered triple and skips semantic validation. Every connected
input also requests Descriptor support (role 8); these operations request no
Control. Cell axes exclude the Result batch prefix. Empty output demand is
stateless and makes no sample Need.

An exact identity matrix can share a legal generic or spatial Result mapping;
`materialize` requests copies. Forced view rejects a statically nonidentity
mapping during compile/direct preflight and rejects an identity mapping whose
physical representation cannot be proven during observation evaluation. Output
is published transactionally under Root budgets and cancellation. The Result
cache is disabled; retained owners follow normal Result lifetime rules. There is
no GPU implementation. Historical Value/planar performance records are not
Result evidence; see the [performance guide](../../examples/rgb_basis_performance/README.md).

`format::convert_linear_rgb` stages a complete expansion and mutates the supplied
WorkflowDocument only after static validation succeeds. Callers must serialize
writes to the same document. Compiled plans and their immutable prepared state
can be executed concurrently. D's policy selects A→B, A→B with `preserve_xyz`,
or A→C→B with an explicit method. The helper returns B's `values` edge and adds
no native D key. `test_rgb_basis_math` and `test_rgb_basis` pass; the integration
suite retains 4,032 independent oracle checks and covers batched inputs, three
Result storage layouts, identity views, concurrent reuse and owner release. The
wide 65,536-channel identity view also passes under a 512 KiB Metadata budget.
The `installed_rgb_basis` consumer and standalone installed-package performance
consumer build pass; a size-3 Float32 D case also passes its full 36-element
strict-reference gate. The 25 benchmark smoke cases pass a same-implementation
strict reference gate, not an independent oracle; no full matrix or speed
conclusion is available.

## FMT-08 metadata assignment and removal

The registry provides three Result CPU keys: `metadata.assign_strict`, `metadata.assign_accelerated_apple_silicon` and `metadata.assign_accelerated_x86_64`. `format::assign_metadata` appends A; `format::remove_metadata` transactionally lowers B to A. The input and output are single-tensor Results with no fields. They preserve schema id, tensor key, batches, descriptor, layout, source publication policy and sample bits. Execution requests same-coordinate Data and Descriptor support with roles 1 and 8, without Validation or Control. The operation never scans sample values and disables Result caching. The public header `photospider/format/metadata.hpp` is installed separately. The [public example](../../examples/metadata_workflow/README.md) verifies special-value bits and source immutability. Five focused Result CTest cases, the example, and two installed consumer checks pass. The metadata-to-extraction configuration/resource chain is covered; composition through `channel.assemble` is not. The older [performance guide](../../examples/metadata_performance/README.md) measures Value/planar execution, not this Result path.


## FMT-11 model conversions

FMT-11 registers 19 native members across three CPU profiles, for 57 model-conversion keys. The installed `photospider/format/model_conversion.hpp` helpers append one key and return the `values` port; FMT-11S appends the registered `mask.threshold_channel_<profile>` operation and has no native color key. All inputs and outputs are single-tensor Results without fields, using Float32 or Float64. The complete sample rank, including batch and cell axes, is at most 8; sample counts follow Result schema representability and execution resource limits.

The `axis` and `output_axis` parameters index cell axes, excluding the batch prefix. Q reduces a selected triple to one component; R expands one Gray component to three, increasing the existing channel-axis extent by two or inserting one length-three cell axis for axis-free input. Outputs preserve schema id, tensor key and batches, while updating model metadata and setting `atomic_trailing_axes` to zero. Semantic selected samples request Data and Validation; raw T validates its binary selector, raw S and bypass samples request Data only, and constant R outputs require Descriptor support only. Empty demand is stateless. Q returns a view only when the complete mapping passes the generic affine or canonical spatial Result view proof; forced view reports `ViewUnavailable` if proof fails, while `auto` materializes. Other model conversions materialize and reject forced view, including bypass-only requests. Publication is transactional, and no GPU profile is registered. Six focused checks and the three installed consumers `installed_model_conversion`, `installed_model_result` and `installed_alpha_model_interop` pass. Sparse Q-view tests require a mapping that satisfies the canonical spatial view proof.
