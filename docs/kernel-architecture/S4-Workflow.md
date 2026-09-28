# Native GPU workflows

`ExecutionMode::NativeGpu` grants placement of operations that declare a native
implementation; `ExecutionContext` selects the configured Metal or Vulkan
device. The mode does not define arithmetic: each operation and profile owns
its numerical contract. Current host paths are exercised by
`test_native_execution`, `test_native_gpu`, and the
[G4 GPU workflow](../../examples/g4_gpu_workflow/README.md). Check support per
registered operation and configured device.

The source under `examples/s4_gpu_workflow` remains a migration fixture. Its
old built-in Gaussian scenario has no default-registry implementation after
05-filter retirement. The independently built `rgba32f` C operation module has
its own registration and resource boundary; loading it does not register the
new FIL specifications.
