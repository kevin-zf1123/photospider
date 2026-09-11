# G4 staged GPU workflow

This public C++ example registers a staged operation, compiles a workflow and
executes exact requests through `ExecutionContext::execute_fragments`. A CPU
control stage discovers 65 separated data samples per observation. Native Metal
reads their atlas using three bindings and explicitly checks missing samples.

Expected results are `2145,4290,2145`: two native dispatches and one pure block
cache hit, with the third observation retaining its own control evidence. Native
cached output/state storage totals 36 bytes. The example also checks a finite
33079-byte admission failure, 33080-byte success and owner reuse, and ordinary
streaming cancellation after a native service failure.

Build in the repository:

```sh
cmake --build build/issue257-static --target photospider_g4_gpu_workflow -j 8
build/issue257-static/photospider_g4_gpu_workflow
```

Or build against an installed Photospider 0.8 package:

```sh
cmake -S examples/g4_gpu_workflow -B build/g4-gpu-consumer -DCMAKE_PREFIX_PATH=/path/to/photospider/install
cmake --build build/g4-gpu-consumer -j 8
build/g4-gpu-consumer/photospider_g4_gpu_workflow
```

Exit 77 means native Metal is unavailable; the CPU oracle still runs. It does
not count as native success. This example covers C++ staged GPU execution;
bounded GPU discovery is provided by the additional workflow below. The C staged bridge is covered below.


The same project builds a C11 plugin and its public workflow loader:

```sh
cmake --build build/issue257-static --target photospider_g4_c_gpu_workflow -j 8
build/issue257-static/photospider_g4_c_gpu_workflow
# Standalone installed-package build also includes these targets:
build/g4-gpu-consumer/photospider_g4_c_gpu_workflow
```

The C version returns `2145,2145` with one native dispatch, one block hit and
16 native cache bytes, and verifies current control evidence. Negative cases
reject stale/forged tokens, malformed native service records, immutable input
promotion, writes after publication and missing shader samples. It uses the same
bounded host services as C++ stages. After each failure, a fresh computation runs
with result caching disabled at the exact declared native stage reservation,
checking owner retirement. An optional argument selects the module path.


The discovery C11 module and loader exercise GPU-generated requests followed by
host supply and native computation:

```sh
cmake --build build/issue257-static --target photospider_g4_discovery_workflow -j 8
build/issue257-static/photospider_g4_discovery_workflow
# Also available in the standalone installed-package build:
build/g4-gpu-consumer/photospider_g4_discovery_workflow
```

Expected values are 8 and 24 with four actual dispatches. A control edit changes
the first value to 24 with new data edges, while frozen execution returns 8.
Overflow, disabled discovery and premature completion fail. The native table's
rounded allocation participates in an exact admission frontier. See
[GPU Discovery](../../docs/kernel-architecture/GPU-Discovery.md) for the wire
format, resource limits and protocol tests.


Existing synchronous GPU producers and CPU fallback are exercised by:

```sh
cmake --build build/issue257-static --target photospider_g4_sync_gpu_workflow -j 4
build/issue257-static/photospider_g4_sync_gpu_workflow
# The standalone installed-package build includes the same target.
```

Expected separated results for `x+1` are 1 and 3. The workflow checks real Metal,
missing-device fallback, Whole and staged restart with native descendants,
rejected attempt diagnostics, cache isolation and exact 65535/65536-byte
admission. No Metal returns 77 only after the CPU fallback checks. See
[Fragment Atlas](../../docs/kernel-architecture/Fragment-Atlas.md#synchronous-producers-and-cpu-fallback).
