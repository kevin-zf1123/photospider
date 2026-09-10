# Composable foundations workflows

The local 0.7 implementation of [ADR 0020](../adr/0020-composable-operation-foundations.md)
provides G1/G2/G3/G5 numeric/semantic/output/scalar contracts and their reusable
operator subset. The delivery target is `ops`; the audited `main@fba06270` stays
package 0.6.0. This implementation status does not claim completed PR CI, external
review or merge. Daemon's existing 0.6 consumer has not been migrated.

The [self-contained example](../../examples/foundations_workflow/README.md) uses
only installed public APIs. Copy its directory outside the repository, install a
static or shared kernel, configure with `CMAKE_PREFIX_PATH`, then run
`photospider_foundations_workflow --scenario all`. Its README gives exact commands,
input/output shapes, parameters, Region behavior, expected results and ways to
modify the graphs. [中文](zh/Foundations-Workflow.zh.md) mirrors this guide.

| Composition | Operation contract |
| --- | --- |
| Cast/range and signed arithmetic/reductions | [Numeric operations](Numeric-Operations.md) |
| Extract/process/merge, alpha and reference-white conversion | [Channel and color operations](Channel-and-Color-Operations.md) |
| Expression sampling, LUT and compile-once dynamic gain | [Expression and LUT](Expression-and-LUT-Operations.md), [bounded image ports](Image-Operations.md) |
| Binary masks, stable labels and fixed-capacity properties | [Component operations](Component-Operations.md) |
| Curve grading, selection feathering, filter statistics and generated fields | [Basic operations](Basic-Operations.md) |
| Image-v2 snapshots, frozen inputs and bounded result retention | [Cache model](Cache-Model.md) |

Ten independently selectable scenarios and `all` are registered in CTest. The
isolated installed consumer builds the same example with only exported targets,
including the matching sanitizer options when applicable. A representative
focused cross-feature command is:

```sh
cmake --build build/issue257-static --target test_numeric_operations test_color_operations test_expression_operations test_component_operations test_basic_operations test_computed_scalar -j 8
ctest --test-dir build/issue257-static -R '^test_(foundations_.*|numeric_operations|color_operations|expression_operations|component_operations|basic_operations|computed_scalar|installed_consumer)$' --output-on-failure
```

Use an existing shared build path for shared consumption. Exact sample and
failure oracles are maintained alongside the scenario source; deeper limits,
strided views, floating environments, cancellation and cache races remain in the
focused integration tests linked by each operator guide. The eight migrated
image operations retain independent CPU/C/Metal acceptance in the
[S4 guide](S4-Workflow.md); a successful CPU example does not assert GPU dispatch.

The retained boundary excludes G4 per-port spatial dependency expansion, G6 host
assets, daemon migration, full paths, FFT, ICC/OCIO and the remaining Proposed
catalogue. New global/shape-changing operations use Whole and one output per node.
