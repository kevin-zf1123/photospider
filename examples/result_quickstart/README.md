# Result quickstart

This example registers one public C++ Result operation, compiles a one-node `WorkflowDocument`, executes it through `ExecutionContext`, and reads the named `answer` Result. The expected output is `answer=42`. The implementation uses only installed public Photospider headers and the `Photospider::kernel` target.

## Build and run

Install Photospider 0.33.0 or a compatible 0.33 release. Then configure and build this directory against that prefix:

```sh
cmake -S examples/result_quickstart -B build/result-quickstart \
  -DCMAKE_PREFIX_PATH=/path/to/photospider-prefix
cmake --build build/result-quickstart -j8
build/result-quickstart/photospider_result_quickstart
```

On success, the program prints:

```text
answer=42
```

The registry is local to the program. It registers the constant operation and freezes before compilation. The operation publishes a one-element Float64 tensor through `ResultBuilder`; the host returns it as a named `ResultRef`. The example reports a nonzero exit status if compilation, execution, descriptor lookup, or tensor read fails.

The installed consumer suite also compiles and runs this example through the exported package target. For package configuration, consumer gates, labels and skip behavior, see [Testing and Validation](../../docs/development/Testing-and-Validation.md).

## Writing this Result operation

The complete, compiled example is in [`main.cpp`](main.cpp), with the standalone target in [`CMakeLists.txt`](CMakeLists.txt). Its operation contract uses a `SchemaTemplate` named `example.scalar` and one `ResultTensorSpec`: key `number`, type `Float64`, cell shape `{1}`. `OperationTraits::outputs[0]` declares a Result output with the same schema id and version, Whole-region execution, `continuation_bytes = sizeof(Constant)`, and one maximum dependency stage. The public declarations are in [`operation_registry.hpp`](../../include/photospider/plugin/operation_registry.hpp) and [`result_program.hpp`](../../include/photospider/plugin/result_program.hpp).

`start_result` asks the host-managed allocator to construct the continuation with `ResultContinuation::make`. The declared continuation size is the budget for its state. Each `poll` receives a `ResultProgramPhase` borrowed for that call. Build the `ResultBuilder` with the phase's resource budget, resolved output schema, and semantic key; the empty-input Cartesian relation describes this constant output's descriptor and tensor support. Bind the descriptor relation, publish tensor slot 0 over the complete `{1}` region, and seal the builder. Returning `ResultPublication{result, true}` marks the sealed result complete. The public builder and relation APIs are declared in [`result.hpp`](../../include/photospider/data/result.hpp) and [`result_relation.hpp`](../../include/photospider/data/result_relation.hpp).

Do not retain the phase, its query, or callback/service views after `poll` returns. Copy an owning `ResultRef` when a result must outlive that poll; the returned workflow Results can also be retained beyond their `ExecutionContext` lifetime. See [Plugin ABI](../../docs/kernel-architecture/Plugin-ABI.md), [Global Results](../../docs/kernel-architecture/Global-Results.md), and [Managed Resources](../../docs/kernel-architecture/Managed-Resources.md) for the callback, ownership, and resource contracts.

The program freezes its local `OperationRegistry` before compiling the workflow. The document names node 1 as `example.constant` and exposes its `value` port as workflow output `answer`; execution then reads tensor slot 0 at coordinate `{0}`. The repository root CMake registers `test_result_quickstart` in its CTest suite. The installed consumer suite registers the same executable as `installed_result_cpp_minimal`, so this source is also compiled and run through the exported package; see [Testing and Validation](../../docs/development/Testing-and-Validation.md).
