# 多输出算子

多输出节点公开一组编译时确定的命名端口。workflow 边通过 `WorkflowNodeOutput{node, port}` 选择端口，根通过 `WorkflowOutput{name, node, port}` 选择端口。编译器在执行前解析端口名和输出 shape。执行阶段只请求有需求的输出区域；未请求端口不计算，启用 joint contract 后，被请求的输出可以共享物理工作。

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

默认 registry 中的图像多输出算子为 `image.split_horizontal`：

| 端口 | Shape | 输入坐标映射 |
| --- | --- | --- |
| `full` | `{H,W,C}` | `(y,x,c)` |
| `left` | `{H,split_x,C}` | `(y,x,c)` |
| `right` | `{H,W-split_x,C}` | `(y,x+split_x,c)` |

输入是 Float32 rank-3 图像。必填 Int64 参数 `split_x` 满足 `0 < split_x < W`，由调用者显式提供。每个输出沿用输入的图像解释。算子为每个请求区域声明来源像素，并发布保留来源存储 owner 的不可变视图。收集稠密结果时可能复制视图。Joint 成员需求相同时，规划器可以共享依赖传输；每个输出仍保留自己的坐标映射。

`image.split_horizontal` 仍注册为 legacy `Value` callback，且未设置 `planar_storage_capable`。带 packed `photospider.image` facet 的 workflow declaration 在输入校验时返回 `InvalidArgument`；structural `PlanarImageLayout` 在算子 capability 检查时返回 `TypeMismatch`。仓库示例会创建前一种 declaration，因此源码可以构建，但执行无法完成。其 README 明确说明它不是当前运行验收。

```cpp
ps::WorkflowNodeOutput left{node_id, "left"};
ps::WorkflowOutput result{"crop", node_id, "right"};
```

`ExecutionOptions::enable_joint` 控制是否为声明了 joint contract 的算子启用可选联合执行。它只影响物理共享；每个命名输出仍有自己的 descriptor、Region 和值语义。注册的 split 定义会在请求来源样本前校验 `split_x`。

宿主集成测试为 [`test_multi_output_execution.cpp`](../../../tests/integration/test_multi_output_execution.cpp)，它使用测试定义的算子核验 named-output 宿主契约。示例源码位于 [`examples/multi_output_workflow`](../../../examples/multi_output_workflow/README.md)；其图像 declaration 会在 split callback 运行前被拒绝。图像存储行为见[图像算子](Image-Operations.zh.md)。

```sh
cmake --build build --target test_multi_output_execution -j 8
ctest --test-dir build -R '^test_multi_output_execution$' --output-on-failure
```
