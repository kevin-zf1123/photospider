# 具名 outputs 与混合 Result ports

## 范围与 ownership

一个 operation node 可声明多个具名 outputs。每个 output 拥有自己的 port kind、descriptor 或 Result schema、input projection 和 execution demand。Workflow edge 同时指定 node 和 output port。Compiler 在执行前专门化选中的 output。

Output 可以是 numeric `Value` 或 structured `Result`。Result schema 可组合 typed image slots 和 primitive fields。Node 可以混合 Result 图像 inputs、numeric Value controls 和不同类型的 outputs。所有图像语义与 ownership 都经 `ResultRef`；`PlanarImage` 是 Result image slot 内的 backing。

```cpp
struct WorkflowNodeOutput final {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};

struct WorkflowOutput final {
  std::string name;
  std::uint64_t node_id = 0;
  std::string port = "value";
};
```

片段省略了无关字段。Public execution 将具名 Value outputs 放入 `ExecutionResult::values`，将 structured outputs 放入 `ExecutionResult::results`。

## Output selection 与 dependency

`OperationTraits::outputs` 声明可用 outputs。`select_operation_output` 解析所选 output 的 traits 和 index；`ResultProgramQuery::output_index` 将该选择传递给 structured callback。所选 output contract、query、schema 和 input projection 定义它的 demand 与 identity。

Coordinator 只解析所选 projection 中的 inputs。Result callback 可请求 numeric Control sample，在下一 stage 检查它，再请求由该值选择的 Data image samples。Result relation 记录已消费的 Control 与 Data support，dirty transpose 使用同一 relation。不属于该 output projection 的 inputs 不会为该 output 执行。

`ExecutionOptions::enable_joint` 可启用注册过的 contract，在符合条件的 requested outputs 之间共享物理 work。每个 output 仍保留自己的 schema、query、relation、cache identity 和 error。Joint execution 不会授权未请求的 output，也不会合并不同的 output 语义。

## Identity 与资源影响

Structured producer 的 identity 包含所选 output、output contract、captured query、parameters 和有序 input bundle。物理 tile 尺寸不改变 semantic output identity。Relation traversal、reads、publication payload、continuation state 和 retained input owners 共用 execution-root budget。

所选 output 失败时，请求返回 typed error。只有注册 contract 允许请求的成员时，符合条件的 joint group 才能共享 continuation。未请求的 output 不会启动。Result owners 在最后一个 owning reference 释放前保持存活。

## 可执行 fixture

[`examples/unified_result_workflow`](../../../examples/unified_result_workflow/README.zh.md) 使用 public workflow APIs 和最小 example operations，从同一 node 选择 Result image output 与 numeric output。Focused `test_multi_output_execution` 和 `test_unified_result_images` 覆盖 output selection、混合 ports 和图像执行。Production operation catalog 与图像限制见[图像 operations](Image-Operations.zh.md)。
