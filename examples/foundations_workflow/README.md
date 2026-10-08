# Foundations workflow example

This C++17 example builds against `Photospider::kernel` and runs the `numeric`, `expression-lut`, and `generator-gain` scenarios. The default `all` selector runs all three. The `generator-gain` scenario is an explicit manual workflow and has no dedicated CTest registration.

| Scenario | Independently checked result |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`; mean of `[1,2,3]` is `2`, variance is `2/3`. |
| `expression-lut` | Sampling `x^2` yields `[0,0.25,1]`; a linear LUT lookup at `0.25` yields `0.125`. |
| `generator-gain` | Reuses one compiled image-gain workflow with new coefficient Results, including concurrent bindings; invalid and nonfinite gains fail. |

All three scenarios declare Result workflow inputs and read returned Result
outputs. Their helpers use `Value` only to create private packed fixture bytes.
The `numeric` and `expression-lut` scenarios check their returned tensors after
execution, and those Results remain readable after their execution context is
destroyed. `generator-gain` reuses a compiled plan for sequential and concurrent
coefficient bindings. When both the gain and image Results remain owned, an
identical frozen request reuses the gain Result without producer timings; invalid
and NaN coefficients remain failures.

From the repository root, build and run the example with:

```sh
cmake --build build/kernel-dev --target photospider_foundations_workflow -j8
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario expression-lut
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario generator-gain
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build/kernel-dev -R '^(test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```

Pass `--scenario numeric`, `--scenario expression-lut`, or `--scenario generator-gain` to run one scenario. The all-scenarios run prints `Foundations scenarios=3 oracle=passed backend=cpu`. CTest registers the numeric and expression/LUT scenarios; run `generator-gain` explicitly to check plan reuse and its concurrent bindings. An error prints `Foundations failed: ...` and exits with status 1.

The standalone CMake project requires an installed Photospider 0.30 package. For example, configure it with:

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```

The consumer links only `Photospider::kernel`. The source-level Result checks and the two registered CTest cases cover the `numeric` and `expression-lut` scenarios; `generator-gain` is checked by its explicit manual run. These workflows do not validate other operation families or native GPU execution.
