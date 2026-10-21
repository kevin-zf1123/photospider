# ADR 0016：将工作流输入绑定到每次执行

- 状态：Accepted

## 决策

Workflow declaration 提供名称与 Result schema；每个 Run 按精确名称绑定 owning
`ResultRef`。编译器在执行前固定输入读取、物理 layout、内存 domain、COPY task
和控制流。Payload owner 不进入编译产物与 identity。

当前 operation module 与 fixed invocation 均使用 ABI 8。算子 lower 为物理 task，
并 prepare 不可变 kernel。Invocation 借用 Plan 固定的 input/output view 和
scratch；payload 不能增加 producer task 或扩大已编译读取。

## 声明与绑定契约

`WorkflowDocument` schema 7 保存 declarations、operation nodes 和具名 outputs。
Declaration ID 唯一且非零；名称是唯一、区分大小写、无空格的可打印 ASCII 字符串，
长度为 1 至 128 字节。每份 document 最多有 4096 个 declarations，每项恰好绑定
一次，包括图未消费的声明。

声明固定所有具名静态 Tensor 成员、dtype、cell shape、batch/atomic 轴和拓扑。
输入结构变化后须重新 preparation 和 compilation。以下片段展示绑定字段，完整
API 以公开头文件为准：

```cpp
struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};

struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};
```

Vector 保留重复名称，供 preflight 拒绝。缺少、多余、重复或非法绑定返回
`InvalidArgument`；Result schema 不兼容返回 `TypeMismatch`。输入 payload 必须
已经覆盖已编译 tile view，并符合其物理 layout 和 domain。Layout 适配必须使用
显式编译的 COPY task；preflight 不进行数值样本校验或隐式 repacking。

`ExecutionContext::prepare` 将 Plan 绑定到具体 kernel 和 device，不执行 payload
工作。`submit(prepared, bindings, cancellation, options)` 复制 binding snapshot，
返回 `RunHandle`；复制期间调用方不得修改 binding 容器。并发 Run 可共享一个
`PreparedPlan`，同时持有不同的不可变 bindings。导入 Plan 使用相同 preflight
规则，并使用其链接的冻结 registry。

## 结果与有序通知

`WorkflowDocument` 输出选择 operation-node output。直接构造 `IRProgram` 的
前端可返回 schema 匹配的 SSA value，包括输入和 block parameter。具名返回对应
物理 CFG 的 return values 与 Plan return signature。

Task 完成的 tile 可在所属 call 退休前满足内部后继。每个 `CallGroup` 在其 tasks
settle 后按 IR call 顺序退休。`ExecutionOptions::on_call_retired` 在独立串行
lane 接收 owning `CallRecord`。其 `values` 以物理 value ID 为 key，成功时包含
call 声明的 exports；失败和 elided call 没有 values。Callback 可保留 Result
owner，超过 Run 或 context 寿命。Callback 抛异常会关闭 delivery，并在已接受
工作排空后 abort Run。Callback 不得 join 自身 Run 或销毁自身 context。

`RunHandle::join` 等待 task quiescence 和 delivery settlement。最终取消及源图
currentness 检查成功后，具名返回才进入 `ExecutionResult::results`。后续 call
失败时，先前已退休的 call export 仍然有效。Observer 与最终 RET 独立持有结果，
其 Result 不保证拥有相同 ObjectId。

## 身份与生命周期

Schema、traits、规范化 selection、固定 reads、layout、placement 和不可变
resource identity 参与对应编译 identity；payload 地址和 binding owner 不参与。
每个 Run 保留 binding snapshot 和已准入 owner，直到已接受的 CPU/GPU 工作完成，
包括取消后的 drain。每个成员有独立 tile grid，见
[ADR 0023](0023-track-dependencies-between-fixed-tiles.zh.md)。

取消停止新发射并排空已接受工作，不会取消共享同一 Plan 的其他 Run。输出和
owning read window 在 context 销毁后继续持有 backing 及其资源计费，最后一个
owner 释放资源。执行器没有跨 Run 的 Result 或 checkpoint cache。

## 边界

- Declaration 不加载文件，也不保留调用方 payload。
- Binding 没有 optional/default value 或隐式 schema conversion。
- Runtime input data 不作为编译期参数或 literal。
- 逻辑样本授权与 backing storage layout 分开定义。
- Preparation 不能把失败的固定 placement 改到其他 backend 重试。
- 本 ADR 定义 C++ workflow binding 和所有权；operation ABI 由公开的 fixed
  invocation 与 module 头文件定义。
