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
decision status. FMT-02..08 remain unimplemented; FMT-07 is retired. Complete images use planar storage and straight color with
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

The public `TensorDescription` codec uses `photospider.tensor-description` v1.
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
