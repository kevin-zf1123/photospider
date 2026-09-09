# S4: Native Metal Image Workflows

The standalone [example](../../examples/s4_gpu_workflow) uses installed public
APIs and the same eight image/mask operations as S1-S3. It selects CPU exact
arithmetic by default. MetalFp32 is explicit, uses Apple Silicon shared buffers,
and falls back per operation when a device or numeric implementation is
unavailable. It does not promise CPU bit identity or an automatic speedup.

## Build and run

From the repository root, install the current kernel and build the independent
example and trusted C operation module:

```sh
cmake --build build/issue257-static --target photospider -j 8
cmake --install build/issue257-static --prefix build/s4-prefix
cmake -S examples/s4_gpu_workflow -B build/s4-example-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s4-prefix"
cmake --build build/s4-example-installed -j 8
cmake -S plugins/ops/rgba32f -B build/s4-module-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s4-prefix"
cmake --build build/s4-module-installed -j 8

build/s4-example-installed/photospider_s4_gpu_workflow --backend cpu
build/s4-example-installed/photospider_s4_gpu_workflow \
  --backend metal --no-cache --explain
build/s4-example-installed/photospider_s4_gpu_workflow \
  --backend metal --layout tiled
build/s4-example-installed/photospider_s4_gpu_workflow \
  --backend metal --layout roi
```

Append `--module /absolute/path/to/libphotospider_rgba32f_ops.so` for the C module
(on macOS the CMake MODULE target also uses `.so`; use the actual generated path).
Only explicitly accepted trusted native code should be loaded. The SDK stays
pure C; host services own device, queue, pipeline and buffer lifetime.

`-DPHOTOSPIDER_ENABLE_METAL=OFF` builds the CPU fallback configuration. Native
support is optional on other platforms. `--require-native` returns 77 when a
scenario could only validate CPU fallback; CTest records that as a hardware
skip. Ordinary example runs still validate and report their fallback result.

## Checkable scenarios

| Scenario | Required observation |
| --- | --- |
| `resident-chain --backend metal --no-cache` | Whole 17x13 fixture: 5 dispatches in 4 submissions, 4 input copies / 7960 bytes, 3536 collected result-copy bytes, zero fallback, `oracle=passed` |
| `resident-chain --layout tiled` | Bounded 4x4 tiles, including clipped edges, compared with the whole-image oracle |
| `resident-chain --layout roi` | Requested Region y=[2,9), x=[3,12), all RGBA channels; correct regional samples |
| `all-operations` | All eight operations, whole and tiled nonzero ROI, positive native dispatches when supported, `oracle=passed` |
| `cache-edits` | Warm dispatches/uploads zero; gain change preserves blur; local stamp recomputes fewer than 20 blur tiles; unrelated edit has no callbacks; bounded native retained bytes |
| `preview-export` | Nine ordered stamps, 20 frozen export tiles, full-quality latest preview, three rejected stale/quality/target publications, independent input/output oracles |
| `fallback` | Disabled-device execution and conservative numeric fallback preserve CPU results/errors; large-coordinate circle coverage stays exact |

Select scenarios with `--scenario NAME`. `--layout`, `--no-cache` and `--explain`
apply to resident-chain. Native acceptance requires actual dispatches; a backend
label or a skipped hardware test is not that observation.

Input-copy counters describe actual copies into native buffers. GPU successors
reuse compatible buffers. Host access to completed shared buffers has no extra
D2H allocation, while collecting tiles into a separate host output really copies
bytes and is reported separately. `--explain` prints candidate physical access
steps and capacity bounds; caches and fallback determine actual work. Native
per-operation and aggregate device time remain separate from host callback and
execute time. Timings are observations without a hardware pass threshold.

## Modify and compose

`image_fixture.hpp::scene(kind)` exposes a WorkflowDocument, bindings and an
independent reference image for each operation. `main.cpp` selects the runtime
and `workflow.hpp` reuses the S3 application coordinator for edits and frozen
export. The S3 scene constructor now accepts an execution mode and tile geometry;
its default remains CPU exact with 4x4 tiles.

The minimal public execution sequence is:

```cpp
ps::GraphContext graph(scene.document);
ps::Compiler compiler(registry);
ps::PlanningOptions planning;
planning.execution_mode = ps::ExecutionMode::MetalFp32;
auto compiled = compiler.compile(graph, planning);
// Check compiled.ok() before accessing the value.
ps::ExecutionContextConfig config;
config.gpu_enabled = true;
ps::ExecutionContext execution(registry, config);
auto result = execution.execute(compiled.value().plan, scene.bindings);
// Check result.ok(); read result.value().values.at("result").
```

Change image pixels and ordinary scalar bindings without recompiling. Change
static Gaussian radius/sigma or shrink factor through the document and recompile.
Connect operation outputs by WorkflowNodeOutput to compose a DAG; requested
output Regions and tile sizes are physical planning choices. FrozenExecution
pins the exact input snapshot during edits. See [Image Operations](Image-Operations.md)
for names, ordered ports, ranges, image profiles, Region rules and the explicit
native numeric eligibility bounds. CPU exact and Metal result caches stay
separate; Metal-mode derived results are excluded from disk persistence.

## Native validation and practical limits

The real M5 tests were run using the installed Xcode tools. API and shader
validation must be enabled before device creation:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build/s4-example-installed/photospider_s4_gpu_workflow \
  --backend metal --scenario all-operations --require-native
```

Apple documents the [validation environment variables](https://developer.apple.com/videos/play/wwdc2020/10616/)
and [GPU capture workflow](https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically).
Captures are a diagnostic option, not an automatically generated delivery artifact.
Each context has one native queue and drains submitted work before retirement,
including cancellation. S4 still waits per operation and validates shared pixels
on the CPU. Reducing those costs, device-private resources, additional backends
and measured automatic placement remain later work.
