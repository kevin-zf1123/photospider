# Multi-output operations

A multi-output node exposes a fixed set of named ports. A workflow edge selects a port with `WorkflowNodeOutput{node, port}`, and a root selects one with `WorkflowOutput{name, node, port}`. The compiler resolves port names and output shapes before execution. Execution requests only demanded output regions; an unrequested port is not evaluated unless an enabled joint contract shares work among requested outputs.

```cpp
#include <cstdint>
#include <string>

namespace ps {
struct WorkflowNodeOutput final {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};

struct WorkflowOutput final {
  std::string name;
  std::uint64_t node_id = 0;
  std::string port = "value";
};
}  // namespace ps
```

The default registry's image multi-output operation is `image.split_horizontal`:

| Port | Shape | Mapping to the input |
| --- | --- | --- |
| `full` | `{H,W,C}` | `(y,x,c)` |
| `left` | `{H,split_x,C}` | `(y,x,c)` |
| `right` | `{H,W-split_x,C}` | `(y,x+split_x,c)` |

The input is a Float32 rank-3 image. The required Int64 parameter `split_x` must satisfy `0 < split_x < W`; callers provide it explicitly. Each output uses the input's image interpretation. The operation declares the source pixels for each requested output region, then publishes immutable views that retain their source storage owner. A dense collection may copy a view. The planner can share dependency transport for joint members with matching demands; each output keeps its own coordinate mapping.

`image.split_horizontal` remains registered with a legacy `Value` callback and does not set `planar_storage_capable`. A workflow declaration with its packed `photospider.image` facet fails input validation with `InvalidArgument`; a structural `PlanarImageLayout` fails the operation capability check with `TypeMismatch`. The repository example creates the former declaration, so it builds but cannot complete execution. Its README explicitly excludes current runtime acceptance.

```cpp
ps::WorkflowNodeOutput left{node_id, "left"};
ps::WorkflowOutput result{"crop", node_id, "right"};
```

`ExecutionOptions::enable_joint` enables or disables optional joint execution for operations that declare a joint contract. It affects physical sharing only; each named output retains its own descriptor, region and value semantics. The registered split definition validates `split_x` before requesting source samples.

The focused host test is [`test_multi_output_execution.cpp`](../../tests/integration/test_multi_output_execution.cpp); it uses test-defined operations to verify the named-output host contract. The executable workflow source is [`examples/multi_output_workflow`](../../examples/multi_output_workflow/README.md); its image declaration is rejected before the split callback runs. Image storage behavior is described in [Image Operations](Image-Operations.md).

```sh
cmake --build build --target test_multi_output_execution -j 8
ctest --test-dir build -R '^test_multi_output_execution$' --output-on-failure
```
