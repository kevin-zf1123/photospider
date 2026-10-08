# FMT-03 public workflow

Build and run from the repository root:

```sh
cmake --build build --target photospider_channel_editing -j 8
./build/examples/channel_editing/photospider_channel_editing
```

Expected output, checked byte-for-byte by the program:

```text
offset ROI (0,1,:) = [8, 21, 31, 0.5]; exact bytes verified
```

`main.cpp` declares FP32 base `[1,2,4]`, external component `[1,2]` and
same-dtype scalar `[1]`. It replaces channel 0 from the external plane and
channel 3 from the scalar, then compiles and executes only the second pixel.
Original channels 1 and 2 pass through. `raw` requires explicit base axis 2;
source structure explicitly distinguishes component and scalar. Samples are
copied without color conversion or alpha arithmetic.

The exported header is `photospider/format/channel_editing.hpp` (also included
by `photospider.hpp`). `swizzle_channels(document, inputs, slots, options)`
selects/repeats base channels or explicit scalar/literal sources.
`replace_channels(document, inputs, replacements, options)` performs simultaneous
assignments to original destination selectors. Input ordinal zero is always
base. Each `ChannelEditInput` carries its actual inferred `OperationMetadata`
and an explicit `ChannelEditStructure`. The helper checks declarations at
construction and emits descriptor assertions for compilation of producer edges.

`ChannelSelector` defaults to index `"0"`; `match="name"|"role"` resolves exact,
case-sensitive unique component metadata. `ChannelLiteral` holds a closed dtype
and exactly its native represented bytes, including floating special patterns
and Int64 endpoints. Repeated equivalent literals share one scalar provider.
Connected scalar values can change between executions of one compiled plan.

`ChannelAssemblyOptions` selects `respect` (default), `raw`, or per-input
`override`, optional target metadata, `auto|view|materialize` (default `auto`),
and `strict` (default) or a host-supported named CPU profile. A propagates source
component descriptions and uniquely remapped complete groups. B preserves
original destination semantics; explicit redefinitions cannot relabel unlisted
slots. Every result is immutable and bit preserving. Canonical images use the
DAG's planar storage/tile geometry; generic constant views may have zero strides.
A forced impossible view fails with `ViewUnavailable`.

All seven native dtypes are supported. Cell rank plus batch rank is at most 8. Input ports and output slots
are bounded to 1024, workflow nodes to 65536, each String parameter to 8192 bytes,
and logical tensors to 2^40 elements; encoded metadata may impose a tighter
limit. Invalid selections/metadata return `InvalidArgument`; mismatched
rank/shape/dtype return `TypeMismatch`. Expansion is transactional, including
allocation exceptions; callers serialize document mutation. No sample I/O occurs
while authoring. Descriptor checks cover all inputs, including unused base/source
channels. Runtime Data and dirty support follow only effective source mappings.

The deterministic integration suite covers dtype/rank/axis combinations,
exact regional dependencies, NaN and signed-zero bytes, changing scalar values,
groups, metadata modes, literal and graph limits, views, cancellation and errors.
