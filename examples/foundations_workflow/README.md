# Foundations workflows

This C++17 example uses the public WorkflowDocument, Compiler and ExecutionContext
interfaces. Package 0.20.0 removes the former cast/range, channels, alpha/color,
image-curve and channel-dependent filter scenes. New FMT specifications remain
unimplemented. The default `all` runs the three maintained generic scenarios:

| Scenario | Independently checked result |
| --- | --- |
| `numeric` | `[3,2,1]-[4,4,4]=[-1,-2,-3]`; mean of `[1,2,3]` is 2, variance is 2/3 |
| `expression-lut` | sampled square `[0,.25,1]`; linear LUT at .25 yields .125 |
| `basic-filters` | asymmetric convolution/correlation and error histogram agree with the fixture oracle |

```sh
cmake --build build --target photospider_foundations_workflow -j 8
build/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build -R '^test_workflow_(numeric_reductions|expression_lut|filter_histogram)$' --output-on-failure
```

Success ends with `Foundations scenarios=3 oracle=passed backend=cpu`.
Use `--scenario numeric`, `expression-lut` or `basic-filters` to run one graph.

For an existing installed 0.24 package, configure this directory with
`cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install`,
then build that directory. It consumes only `Photospider::kernel`.

Explicit `generator-gain`, `components` and `basic-masks` selectors remain as
legacy typed-image/mask migration sources. They are excluded from `all` and the
active acceptance set; their former typed paths are not evidence of planar
support. The [retirement record](../../docs/built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)
lists removed operations. No replacement format conversion is supplied here.
