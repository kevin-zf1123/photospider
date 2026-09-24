# Channel and color operations

Package 0.20.0 retires the old channel, alpha and color operations, together with
`numeric.cast` and `numeric.encode_range`. Their source, default registrations
and dedicated old tests are removed. The 13 exact keys and ownership boundaries
are listed in the [retirement record](../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md).
Lookup, invocation and compilation of those keys return `NotFound`; there are
no aliases or registered compatibility stubs.

The [format/color catalog](../built-in_ops/02-format-color/representation.md) and
[FMT common contract](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
define the replacement direction. FMT-01A/B now have CPU implementations and
FMT-01C has a public authoring helper. Their specifications retain Proposed
decision status. FMT-02A/B/C are also implemented. FMT-03A/B have public composition helpers. FMT-04..08 remain unimplemented; FMT-07 is retired. Complete images use planar storage and straight color with
alpha inside the tensor. New operators must implement their own exact demand,
metadata, numerical and layout contracts. Historical typed HWC behavior does
not constitute a subset implementation of these new specifications.

Shared ColorArray descriptions, profile ownership and mathematics used by
maintained NUM/CRV operations remain. Their presence does not register a format
conversion or imply support for the new FMT families.

The [public retirement regression](../../tests/integration/test_format_color_retirement.cpp)
checks all removed keys through lookup, direct invocation and compilation,
and executes `numeric.add_strict` with an independently checked result of 0.5.
The installed consumer builds and runs the same source:

```sh
cmake --build build --target test_format_color_retirement -j 8
ctest --test-dir build -R '^(test_format_color_retirement|test_installed_consumer)$' --output-on-failure
```

The [Chinese mirror](zh/Channel-and-Color-Operations.zh.md) describes the same boundary.

## FMT-01 channel extraction

`channel.extract_index_{strict,accelerated_apple_silicon,accelerated_x86_64}`
selects a static `index`. `channel.extract_named_` with the same profile suffixes
selects an exact unique `selector` in the `match=name|role` namespace. The named
CPU profile must match the host. All profiles copy identical bytes for UInt8,
UInt16, Int8, Int16, Int64, Float32 and Float64, including NaN payloads.

Both entries accept one tensor `input` and return `values`. Direct nodes specify
`metadata_mode=respect|raw|override`, `keepdims` and `layout=auto|view|materialize`.
`axis` is a nonnegative Int64 below rank, required for raw or undescribed input.
Respect mode checks an explicit axis against metadata. Override requires
`metadata_override`, encoded by `tensor_description_parameter`. Selectors are
static; rank is 1..8 and removing an axis requires rank >= 2. No color conversion,
alpha normalization or floating arithmetic occurs.

The public `TensorDescription` codec uses `photospider.tensor-description` v2 (package 0.21.0 rejects v1).
It records explicit axes, ordered names/roles/units, component interpretation and
optional owned ICC identity. Projection retains applicable interpretation and
resources without asserting that one component is a complete color. An override
is invocation-local. `format::split_channels` expands A nodes and returns c0, c1,
... output handles after validating the supplied producer metadata at compilation.

Generic tensors use exact Dependency fragments, including negative/zero strides.
Planar structural channel views retain source virtual backing and restrict valid
coverage to the requested ROI; their execution admission lives until the last
alias retires. Materialization provisions only requested output pages. Raw spatial
selection materializes: keepdims retains planar storage, while removing a spatial
axis produces a generic tensor in `ExecutionResult::values`. A forced unavailable
view returns `InvalidArgument/InvalidDomain` with `ViewUnavailable` during execution.
Generic consumers after this boundary execute with regional source bindings.
Multiple named planar roots execute with independent requested regions.

The runnable public workflow is
[`test_channel_extraction.cpp`](../../tests/integration/test_channel_extraction.cpp).
Its first fixture describes stored B/A/R/G bytes of shape [2,2,4], requests the
second row, and verifies alpha [75,99], named red [12,13], and split handle c2
[12,13]. Additional independent byte oracles cover all seven dtypes, rank eight,
CHW/HWC, cross-tile ROIs, raw spatial selection [105,106,109,110], ownership,
profile overrides, cancellation and disjoint roots.

```sh
cmake --build build --target test_channel_extraction -j 8
ctest --test-dir build -R '^test_channel_extraction$' --output-on-failure
```

Success is exit code 0 with every explicit oracle assertion passing. The same
source is the `photospider_channel_extraction_consumer` installed-package target.
CPU correctness is validated on the native host; this does not establish another
ISA or GPU support. Native index-extraction measurements and Instruments findings
are in the [performance workflow](../../examples/channel_extraction_performance/README.md). Mixed
planar/generic execution currently recompiles each generic node; its diagnostics
include node timings but do not provide a unified per-run memory peak across child
executions. These diagnostics are not RSS measurements.


## FMT-02 channel assembly

Package 0.21.0 implements the following independent primitives, each with
`_strict`, `_accelerated_apple_silicon`, and `_accelerated_x86_64` suffixes:

| Key prefix / authoring helper | Static structure | Output |
| --- | --- | --- |
| `channel.assemble` / `format::assemble_channels` | Equal single-component input shapes; insert `axis` in [0,rank]. Input rank 1..7. | Input order along the new axis. |
| `channel.concatenate` / `format::concatenate_channels` | Resolve a channel axis per input; equal ordered nonchannel extents. `output_axis` in [0,rank). | Ordered channel blocks, rank 1..8. |
| `channel.assemble_mapped` / `format::assemble_mapped_channels` | Explicit component/channels structure per input and static source/destination rows. | Complete destination range [0,row_count), source omissions/reuse allowed. |

All have ordered repeated tensor inputs (1..1024, the kernel port bound), one
`values` output, identical input/output dtype, and exact element-bit copies for
UInt8/UInt16/Int8/Int16/Int64/Float32/Float64. No broadcast, squeeze, conversion,
resampling, normalization, premultiplication or sample-domain validation occurs.
The native accelerated key requires its corresponding ISA; the other named ISA
fails explicitly. The portable strict key works on both tested hosts.

The helpers are in `photospider/format/channel_assembly.hpp`, also included by
`photospider.hpp`. They return a connectable `WorkflowNodeOutput`; callers add
an explicit named root. They serialize `metadata_mode=respect` and `layout=auto`
by default. Direct nodes must supply these two fields and the output axis.
All structured parameters use existing String values, bounded by the shared
8192-byte parameter limit. There is no new operation C ABI parameter kind.

| Parameter | Canonical encoding |
| --- | --- |
| `input_axes` (B, optional) | `v1;axis;_;axis`, one entry per input. `_` resolves metadata; raw requires every axis. Integers are unsigned decimal without leading zeros. |
| `input_structure` (C, required) | `v1;c;h2;h_`. `c` is component with no effective channel axis; `hN` asserts channel axis N; `h_` resolves metadata. |
| `input_overrides` | `v1;ordinal:description_hex;...`, strictly increasing unique valid ordinals. Required nonempty exactly in override mode. Each replaces one input's effective description for this call. |
| `output_description` | `tensor_description_parameter(description)`, canonical lowercase hex of TDM2. Shape/channel-axis/group assertions must agree with inference. |
| `mapping` (C) | `v1;input,match,selector_hex,destination,component_hex;...`. Match is `index`, `name`, or `role`. Selector is strict UTF-8 encoded as lowercase hex, including decimal index text. Component is `_` or a TDM2 description containing only `component`. |

C validates every destination exactly once and evaluates in destination order;
record order never introduces overwrite precedence. Name/role resolution is
case-sensitive and unique, before payload reads. Raw permits only index selection.
Overridden source descriptions affect selectors; destination assignment does not.

Tensor-description v2 adds `TensorInterpretation` to individual components and
explicit `TensorColorGroup` records. The existing global fields remain descriptive
source provenance. Groups explicitly list indices, corresponding components,
interpretation, and optional internal alpha index. Complete groups validate model
roles and required structural interpretation. The canonical model/role vocabulary
is: rgb (red/green/blue), xyz (x/y/z), cielab and oklab (l/a/b), cielch and oklch
(l/c/h), hsl (hue/saturation/lightness), hsv (hue/saturation/value), ycbcr (y/cb/cr),
xyy (x/y/luminance), cmyk (cyan/magenta/yellow/black), gray and black_white (gray).
RGB requires transfer plus named primaries or explicit primaries/white; XYZ/Lab/LCh
require white; CMYK requires an owned profile. These records describe copies and
are not implementations of the corresponding model conversions. Source provenance
may use other model text without declaring a complete group.

Explicit target component/group fields authorize local reinterpretation. Conflicting
fields between target records/groups/channels fail. Unchanged applicable fields
still obey respect; changing only a display name cannot excuse conflicting source
references. A model replacement removes incompatible model-dependent provenance.
No complete group is guessed. Duplicate names/roles remain duplicates. Coordinate
axis assertions are checked on every connected input, including unused ports.
Raw preserves applicable per-component descriptions without consuming their
semantics or propagating incompatible common grids. All referenced ICC resources
are admitted and retained by the result.

The static `DependencyMapPiece` relation supplies exact Data support and inverse
dirty fan-out. Generic execution supports disjoint Footprints and arbitrary legal
positive/negative/zero strides. Planar execution partitions each requested output
into exact source pieces, skips unselected producers, deduplicates repeated source
selections, and publishes one transactional output window. Its internal rectangular
planning envelopes are scheduling bounds only; source reads use exact pieces.
Multiple named image roots can request separate offset regions. No completed
sample-only result cache is enabled for this family.

`auto` proves a legal retained view or materializes. Generic views require one
common owner and affine address map. Planar views require consecutive physical
planes of one owner, matching spatial maps and individually authorized source
coverage; source ranges retain their backing and budget admissions. Reordered,
repeated or unrelated planar sources materialize, and forced `view` reports
`ViewUnavailable`. Materialized image output uses the source's first structural
plane order, the DAG tile geometry and canonical output offsets. Sparse output
prepares only the exact required page union. Generic inputs may participate in a
planar assembly when their nonchannel dimensions agree.

### Runnable public fixture

```cpp
ps::format::ChannelAssemblyOptions options;
options.metadata_mode = "raw";
options.layout = "materialize";
auto merged = ps::format::assemble_channels(document, {red, green, blue}, 2,
                                             options);
if (!merged.ok()) return merged.status();
document.outputs = {{"rgb", merged.value().source_node, "values"}};
// Compiler and ExecutionContext execute this document with explicit bindings.
```

The complete executable is
[`test_channel_assembly.cpp`](../../tests/integration/test_channel_assembly.cpp).
It creates actual sources and compile/execute bindings, and checks every sample
with independent coordinate and byte-address calculations. Deterministic raw
Float32 tests include signed zero, Inf, quiet/signaling NaN payloads and
negative/zero strides. Tests also cover C dirty fan-out, gaps and duplicate-source
read deduplication, Gray-to-explicit-RGB relabeling, Lab component reinterpretation,
metadata/shape errors, shared-owner survival, mixed generic/planar chains,
resource limits, overrides and ICC owner retention.

```sh
cmake --build build --target test_channel_assembly test_channel_extraction \
  test_planar_image_workflow test_compiler -j 8
ctest --test-dir build -R '^(test_channel_assembly|test_channel_extraction|test_planar_image_workflow|test_compiler)$' --output-on-failure
```

The expected result is four passing tests. The C mapped image test publishes
only channels 0 and 2 within [127,130) x [127,130), maps [2,0,2], and requires
18 source bytes for 27 output UInt8 samples; missing channel 1 is never read.
The installed target `photospider_channel_assembly_consumer` runs the same public
fixture using only the installed kernel target. Measured CPU latency, backing,
Instruments hotspots and Linux/x64 validation are in the
[performance guide](../../examples/channel_assembly_performance/README.md).


## FMT-03 channel editing

`format::swizzle_channels` and `format::replace_channels` are exported in
`photospider/format/channel_editing.hpp`. Both lower transactionally to
`channel.assemble_mapped_<profile>`; no native swizzle/replace operation or legacy
alias is introduced. The [minimal public workflow](../../examples/channel_editing/README.md)
executes an offset ROI and verifies `[8,21,31,0.5]` byte-for-byte.

Each input includes actual inferred metadata and explicit channel/component/scalar
structure. Input zero is the base channel tensor, whose axis and nonchannel grid
fix the output. A selects, repeats, omits or fills slots. B replaces distinct
original destination selectors simultaneously and leaves all unlisted samples
and semantics intact. Named selectors use exact unique name/role matches; raw
uses explicit axes and indices. All seven dtypes and positive rank 1..8 preserve
sample bits, including NaN payloads and signed zeros. Literal dtype and bytes are
explicit, with no Float64 intermediate. No alpha arithmetic is performed.

A propagates source component meaning and keeps only uniquely remapped complete
groups and companions. B computes complete destination descriptions, preventing
replacement-source metadata from leaking into unspecified destination fields.
Explicit groups replace overlapping or same-name groups; unaffected valid groups
survive. Raw drops descriptions incompatible with the explicitly selected axis.
Override remains invocation-local. Coordinate compatibility applies to every
connected spatial input, including unused ones; no implicit resampling occurs.

The FMT-03 lowerer fuses generic scalar fills into C's internal `s` source records.
A scalar maps every selected output coordinate to its sole index zero; ordinary
FMT-02C's public `ChannelSourceStructure` remains component/channels only. Generated
C nodes carry `authoring_member`, lossless bounded `expected_inputs` assertions and
`output_description_complete`. These internal records preserve the supplied
Descriptor checks while enabling exact fixed-coordinate Data/dirty mappings.
They participate in the normal compiler invocation identity. The public FMT-03
helper validates original declarations and compilation rechecks producer metadata.
All-constant output retains base Descriptor support without base Data reads.

Equivalent typed literals share one `channel.scalar_literal_<profile>` provider
with Int64 `dtype` (one of the seven ElementType values) and lowercase String
`bits` (exact native-order bytes). It returns generic shape `[1]`, port `values`,
with empty facets and scalar-sized owned storage. It has no inputs and uses
Whole semantics over one sample. Invalid dtype/width/hex fails preflight. A/B
introduce one C node plus the actual unique literal providers, with collision-aware
IDs and a 65536-node limit. Scalar fills add no spatial temporary allocation.

Generic single-owner affine results can retain zero-stride scalar views; independent
owners require materialization. Canonical images cannot alias scalar storage.
The mapped planar path reserves the whole output virtual span and backs only
requested pages. Exact source demand and valid coverage remain independent of
page/tile rounding. The optimized scalar copy doubles initialized byte prefixes
inside blocks of at most 1024 samples, with cancellation/currentness checks and
one atomic output publication. It performs no floating arithmetic or unrequested
neighbor reads. No private worker pool or completed-result cache is added.

Build `test_channel_editing` and run its focused CTest for the full integration
fixture; `photospider_channel_editing_consumer` runs it against an installed public
package. See [performance results](../../examples/channel_editing_performance/README.md)
for FP32 128x128, 4096x4096 continuous/tiled, sparse requests, representation,
copy/backing accounting and the measured optimization. Runtime observations are
separate from the retained Proposed specification decision status.
