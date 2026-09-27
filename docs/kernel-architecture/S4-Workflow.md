# Native Metal workflows

`ExecutionMode::MetalFp32` and the native GPU services remain available to
operations that explicitly support them. The current host paths are exercised
by `test_native_execution`, `test_native_gpu`, and the
[G4 GPU workflow](../../examples/g4_gpu_workflow/README.md). Backend support
must be checked per registered operation and does not extend to the proposed
05-filter members.

The source under `examples/s4_gpu_workflow` remains a migration fixture. Its
old built-in Gaussian scenario has no default-registry implementation after
05-filter retirement. The independently built `rgba32f` C operation module has
its own registration and resource boundary; loading it does not register the
new FIL specifications.
