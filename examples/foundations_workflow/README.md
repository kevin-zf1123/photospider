# Composable foundations workflow

This directory is a self-contained C++17 consumer of an installed Photospider
0.7 kernel. Copy it anywhere, point CMake at the installation and run. It uses
only `photospider/photospider.hpp`, WorkflowDocument, Compiler, ExecutionContext
and other public package APIs. No repository test helper, internal header, media
file, daemon or plugin source is required. English is authoritative;
[中文说明](README.zh.md) describes the same example.

## Build and run

From the kernel repository root, configure, build and install the package, then
build this example against that installation:

```sh
cmake -S . -B build/foundations-kernel -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF
cmake --build build/foundations-kernel --target photospider -j 8
cmake --install build/foundations-kernel --prefix "$PWD/build/foundations-prefix"
cmake -S examples/foundations_workflow -B build/foundations-example \
  -DCMAKE_PREFIX_PATH="$PWD/build/foundations-prefix"
cmake --build build/foundations-example -j 8
build/foundations-example/photospider_foundations_workflow --scenario all
```

For a shared kernel, add `-DBUILD_SHARED_LIBS=ON` to the kernel configure command.
After copying this example directory outside the repository, enter the copied
directory and build it using an absolute path to the installed package:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/foundations-prefix
cmake --build build -j 8
build/photospider_foundations_workflow --scenario all
```

The executable requires no runtime data files. Exit zero means every selected
independent oracle passed; an error prints its reason and
returns one. `--help` lists scenarios; `all` is the default. CPU exact mode is
explicitly retained for these foundation examples. Native image acceptance is
a separate [S4 workflow](../s4_gpu_workflow).

## Scenarios and expected results

Run each row with `--scenario NAME`:

| Name / source | Input and composition | Checkable result |
| --- | --- | --- |
| `cast-range` / `numeric.cpp` | All UInt8 0..255, encode 0..255→0..1→0..255, cast round-trips; Float64 halfway/boundary/nonfinite inputs; Float32 subtraction/reductions | 256 exact bytes; ties `[-2,-2,0,0,2,2]`; explicit overflow rejection/clip; `[-1,-2,-3]`; mean 2, population variance 2/3 |
| `channels` / `color.cpp` | Premul linear-D65 RGBA `[-2,3,4,.5]`; extract, multiply red, explicit merge; BGR swizzle round-trip | Identity exact; red doubled `[-4,3,4,.5]`, other channels unchanged; BGR round-trip exact |
| `alpha-color` / `color.cpp` | Signed/HDR straight RGBA, alpha .5 and 1e-30; associate/unassociate; hidden color at alpha zero; RGB→XYZ→Lab round-trip; declared D65 and D50 XYZ/Lab | Finite round-trips within 1e-5 relative/absolute scale; alpha bits unchanged; hidden zero-alpha becomes `[0,0,0,0]`; invalid units/facets/association rejected |
| `expression-lut` / `generators.cpp` | Float64 coefficients; x² sampled at 0,.5,1; table queried at .25; same key with count 5 | `[0,.25,1]`, `.125`, count-5 `[1,1.5,2,2.5,3]`; invalid AST/nonfinite subexpression rejected |
| `generator-gain` / `generators.cpp` | Snapshot RGBA and dynamic Float64 coefficient; count-one `c[0]*2` feeds exposure gain; one plan run sequentially/concurrently | Independent results for each coefficient; actual warm cache hits; invalid fresh/cached gain and NaN coefficients enter gain callback zero times |
| `components` / `components.cpp` | Empty mask; connected bridge; 3x3 diagonal checkerboard; signed field→threshold→labels→count | Zero empty labels/count and nonempty zero tables; bridge area 5/bbox `[0,0,3,2]`; stable diagonal IDs `[1,0,2,0,3,0,4,0,5]`; capacity overflow rejected |

The final line is `Foundations scenarios=6 oracle=passed backend=cpu` for all.
Every success line is printed after comparing computed samples against independent
constants or fractions. Cache-hit counts are actual diagnostics and may vary.

All new numeric/channel/color/expression/LUT/component operators use Whole,
including the component scene's 1x1 planning tiles: connectivity covers the whole
input. Every Value has nonzero shape and complete coverage. Existing exposure
gain keeps its image Region rule and validates the complete Float32 `{1}` scalar
before consumption. Sampled-signal domain units are separate from sample units;
the example uses dimensionless values and axes. Color conversions keep signed
results and never silently clamp a gamut or adapt reference whites.

## Modify and compose

`workflow.hpp::document()` declares inputs and names one result; `bindings()`
creates per-run values. `evaluate()` calls public compile and execute. It is a
small convenience helper, so a workflow can be inspected and changed directly.
For compile-once use, follow `generator_gain()`: keep GraphContext, the compiled
plan and ExecutionContext, then replace only same-descriptor bound Values.

- Add a `WorkflowNode` with a registry key and explicit typed parameters.
  Connect source inputs with `WorkflowInputReference`, upstream outputs with
  `WorkflowNodeOutput`. The output name on each operation is `value`.
- Change static parameters, coefficient-table shape, expression count, merge
  descriptor or component capacity by changing the document and recompiling.
  Dynamic coefficients and image contents can change per run without recompiling.
- Use `encode_semantic`, `semantic_parameter` and `channel_indices_parameter`
  to construct canonical public metadata and static parameters. Do not handwrite
  semantic bytes or assume matching shapes establish image meaning.
- Select named final outputs through `doc.outputs`. Each node has one output;
  branch labels to separate count/area/bbox nodes when multiple properties are
  needed. Capacity is independently declared per node; unused slots remain zero.

Required cast parameters are `dtype`, `rounding="ties_even"`, and
`overflow="reject"` or explicit `clip`; range adds increasing source/destination
endpoints. Arithmetic uses equal Float32/64 dtype/shape without broadcasting.
Built-in channel merge takes 2..4 equal-dtype/equal-shape HW inputs and establishes
its explicit target: Image requires Float32 and 3/4 channels, VectorField accepts
Float32/64 and 2/3 channels, and ComplexField accepts Float32/64 and 2 channels.
Expressions require
`expression/count/start/step`; LUT requires `out_of_domain="reject"` or `clip`.
Threshold explicitly supplies `.5`; component capacities are Int64 `[1,2^53-1]`.
No constructor defaults are silently inserted by the registry.

The gain scene's public registry wrappers count callback entries, delegating to
the default registry with unchanged traits. Its snapshot enables regional reuse;
compact Whole coefficient Values fit the 2048-byte direct-binding cache limit.
The producer-only warm-up deliberately caches a valid dimensionless signal 20;
its subsequent use as gain fails the `[0,16]` consumer constraint.

Full parameter/error contracts and focused regressions are linked from the
[foundations guide](../../docs/kernel-architecture/Foundations-Workflow.md).
This example delivers the accepted subset on the `ops` line; daemon's 0.6 consumer
migration, G4/G6, FFT, full paths and ICC/OCIO remain separate work.
