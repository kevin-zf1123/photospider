# ADR 0016：将工作流输入绑定到每次执行

- 状态：Accepted

## 1. 核心摘要 (TL;DR)

工作流声明提供名称和 Result schema；每次执行按名称绑定一个 owning `ResultRef`。Tensor shape 与 layout 保留在 Result schema 中，payload owners 不进入 compiler identity。每个 operation 都通过 staged Result continuation protocol 执行。

## 2. 架构心智模型 (Mental Model & Intuition)

```text
WorkflowDocument                         每次 execute 调用
  names + Result schemas                       Result bindings
          |                                         |
          v                                         v
       analyze -> optimize -> plan           schema/name preflight
                                      +----------+-----------+
                                      |          |           |
                                  Result A   Result B   Result C
                                      |          |           |
                                      +----------+-----------+
                                                 v
                                      Result continuations
                                                 |
                                   certified outputs 或 failure
```

声明固定 workflow port 需要的 input name 和 Result schema。每次 `execute` 调用提供 schema 对应的 `ResultRef`。Result 在 execution 或返回的 view 仍需要时保留 tensor backing 和 resource owners；并发调用可以复用同一个 plan，同时使用不同的 immutable bindings。

## 3. 契约规约与接口 (Formal Contracts & APIs)

```cpp
struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result = {};
};

struct ExecutionOptions final {
  std::function<Status(ValueRef, const ResultRef&)> result_publication = {};
};

struct ExecutionResult final {
  ResourceMap<ResultRef> results = {};
};
```

这些片段展示 binding 与 publication 相关的公开字段。当前 `WorkflowDocument` schema 为 7，保存 declarations、operation nodes 和具名 outputs。Declaration id 唯一且非零；name 为唯一、精确、区分大小写的可打印 ASCII 字符串，长度为 1 至 128 字节且不含空格。每份 document 最多包含 4096 个 declarations，每个 declaration 恰好绑定一次，包括未被图消费的声明。

声明固定所有具名静态 Tensor 成员及其 dtype、cell shape、batch/atomic 轴和拓扑。输入结构改变后重新 preparation 和 compile。每个 Run 固定 plan、binding snapshot 和 generation。Payload 地址不进入编译 identity；schema、traits、stage 声明和规范化成员请求进入相应 identity。

`ExecutionBinding` 按名称匹配一个 declaration，并持有 Result reference。Executor 按多重集合验证名称，因此重复 binding 会被保留并在校验时失败，不会被静默覆盖。Executor 在调度 operation work 前验证 Result schema 兼容性。Run 会将 binding snapshot 和已准入 input owners 保留到 callbacks 退出。`ExecutionContext::execute` 在 callbacks 退出，且 cancellation 与 graph-currentness checks 允许完成后，将具名 Results 放在 `ExecutionResult::results` 中返回。

`ExecutionOptions::result_publication` 是每次调用独立的 certified Result prefix observer。每个 caller 有独立的串行通知流。Caller 可在 callback 返回后继续保留 owning `ResultRef`。Prefix 不代表整个执行成功；observer 失败停止 Run，此前已认证 prefix 继续有效。Callback exceptions 转换为 typed execution failures；cancellation 和 callback retirement 遵循 Run 现有顺序。

## 4. 负面清单与边界 (Non-Goals & Explicit Boundaries)

- Workflow declarations 不加载文件，也不保留调用方 payload。
- Bindings 没有 optional/default values，也不执行隐式 schema conversion、resize 或 layout repacking。
- Runtime input data 不会成为 compile-time parameters 或 literal nodes。
- Workflow output 选择 operation-node output；输入直接透传不是 workflow output 形式。
- Result tensor backing layout 不改变逻辑 sample authorization。Planar storage 不等同于 interleaved storage。
- Result C operation table 保持 ABI 6。本 ADR 描述 C++ workflow binding contract，不描述独立 C++ Tensor operation protocol。

## 5. 后果与代价 (Consequences)

声明或绑定名称非法返回 InvalidArgument，schema 不兼容返回 TypeMismatch，均发生在 callback 前。每个输入成员都有独立 tile 网格，按成员 tile 请求和比较。同一 backing 可服务不同 tile extent 的 plan。增加生产覆盖或 revision 本身不表示内容编辑。调用方不得在 execute 复制 binding 时修改其容器。

Cancellation 采用协作方式。Executor 会等待已进入的 callbacks 退出后再释放借用 phases 和 owners。每个 Run 拥有自己的 producer；取消一个 Run 会停止并退休该 Run 的工作，不影响其他 Run。输入 Results 保留各自的 backing owners；Root 计费的工作状态、outputs 和 transfers 会持续计费到最后一个 owner 退出。
