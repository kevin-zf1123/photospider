# Foundations workflow example

This C++17 example builds against `Photospider::kernel` and runs the maintained `numeric` and `expression-lut` scenarios. The default `all` selector runs both. The executable also accepts `generator-gain`, but the planar storage gate rejects its typed-image snapshot import, so it is not a runnable current scenario and is excluded from `all` and CTest.

| Scenario | Independently checked result |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`; mean of `[1,2,3]` is `2`, variance is `2/3`. |
| `expression-lut` | Sampling `x^2` yields `[0,0.25,1]`; a linear LUT lookup at `0.25` yields `0.125`. |

From the repository root, build and run the example with:

```sh
cmake --build build --target photospider_foundations_workflow -j 8
build/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build -R '^test_workflow_(numeric_reductions|expression_lut)$' --output-on-failure
```

Pass `--scenario numeric` or `--scenario expression-lut` to run one maintained scenario. The executable prints `Foundations scenarios=2 oracle=passed backend=cpu` after both pass. An error prints `Foundations failed: ...` and exits with status 1.

The standalone CMake project requires an installed Photospider 0.30 package. For example, configure it with:

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j 8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```

The consumer links only `Photospider::kernel`. The source-level result checks and the two registered CTest cases cover the CPU scenarios above; they do not validate other operation families or native GPU execution.
