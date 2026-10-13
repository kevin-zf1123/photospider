# Foundations workflow example

| Scenario | Independently checked result |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`; mean of `[1,2,3]` is `2`, variance is `2/3`. |
| `expression-lut` | Sampling `x^2` yields `[0,0.25,1]`; a linear LUT lookup at `0.25` yields `0.125`. |

From the repository root, build and run the example with:

The standalone CMake project requires an installed Photospider 0.32 package. For example, configure it with:

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```