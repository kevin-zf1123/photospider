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
bounded GPU discovery and the C staged GPU bridge remain separate G4 work.
