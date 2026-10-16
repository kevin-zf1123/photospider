# ADR 0021：独立路由命名结果

- 状态：Accepted
- 英文权威文档：[ADR 0021](../0021-independent-node-results.md)
- 当前边界：原决策中每个输出的 facet 集已被取代；每个输出携带解析后的结构化 Result schema。参见 [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、[结构化 Result 与 tensor slot](../../kernel-architecture/zh/Global-Results.zh.md) 和 [Tensor 描述](../../kernel-architecture/zh/Tensor-Semantic-Metadata.zh.md)。

## 1. 核心摘要（TL;DR）

一个 operation node 可以产生多个命名结果，每个结果都有自己的元数据、依赖和错误路径。Compiler 和 executor 用 `ValueRef` 标识每个结果，因此未使用的 sibling 不会为已选结果增加工作。

## 2. 架构心智模型（Mental Model & Intuition）

Compiler 根据静态输入元数据和参数解析每个输出的 Result schema。执行只沿着有需求的结果端口及其相关输入边推进。每个被请求的结果拥有自己的任务、读取、依赖记录、缓存身份和结果。

```text
                  一个 operation node
                  /       |         \
             output A  output B   output C
                 |         |          |
             demand A  demand B     未选择
                 |         |       不执行
              A 的任务  B 的任务
                 |       /       \
              发布 A  success B  error B
                                    |
                           只失败 B 的消费者
```

`ValueRef` 是 `(node_id, output_index)`。它独立于物理 PlanStep 索引路由结果。每个结果按成员 tile计算和记录，见 [ADR 0023](0023-track-dependencies-between-fixed-tiles.zh.md)。

## 3. 契约规约与接口（Formal Contracts & APIs）

```cpp
struct SemanticOutput {
  std::string key;
  std::shared_ptr<const SchemaTemplate> result_schema;
};

struct WorkflowNodeOutput {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};

struct WorkflowOutput {
  std::string name;
  std::uint64_t node_id = 0;
  std::string port = "value";
};

struct OperationOutputTraits {
  std::string key = "value";
  std::optional<std::vector<std::uint32_t>> input_indices;
};

struct OperationTraits {
  std::vector<OperationOutputTraits> outputs;
};

class Compiler {
 public:
  Result<ExecutionPlan> plan(const OptimizedGraphIR&,
                             const PlanningOptions&) const;
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings,
                                  const CancellationToken& = {},
                                  const ExecutionOptions& = {});
};
```

以上 class 摘录省略了其他无关成员。

这些 C++ 声明展示结果路由字段，省略了外围类型和校验。`OperationTraits` 以声明顺序保存输出。每个输出都有唯一名称及独立推断的 Result schema。C operation descriptor 采用相同的有序模型，最多包含 64 项。纯度约束可选缓存和重试；有副作用算子声明 mandatory 共享 stage。

Workflow 边按名称选择精确的 producer 端口。调用方可见的 `WorkflowOutput` 选择 node 与 port，再为结果指定唯一标签。Compiler 将输出名解析为声明顺序索引，并用 `ValueRef` 记录结果。静态推断使用完整输入元数据；输出的 `input_indices` 投影决定其可执行输入祖先和 callback 视图。投影后的 C callback 输入仍携带原始 schema 索引。

每个有需求的结果由一个单输出 `PlanStep` 表示。未选择的纯输出不会增加执行需求。因此，同一个 node 在调用方请求多个输出时可以对应多个 PlanStep，每个步骤都携带该结果自己的契约和身份。C++ invocation 与 C callback query 会收到声明顺序的 `output_index`。

一个结果的任务失败时，只让该结果及实际依赖它的消费者失败。公开 `execute` 对完整的请求输出集合仍返回一个聚合成功或错误。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 独立输出保留各自投影与 tile demand。显式准备后的共享 stage 可在 Run 内按原始节点/stage 联合计算一次；定义、投影和 schema 不由第一个请求者决定。Mandatory stage 和固定发布 bundle 可扩大必需工作。不支持动态输出数量。

## 5. 后果与代价（Consequences）

输出元数据和结果身份包含被选输出契约、解析后的 Result schema、静态参数及实际输入依赖。改变输出角色、shape 或相关输入投影会改变其身份。无关 node id 和整张图的身份不能替代被选输出的内容依赖。

每个发布结果都拥有自己的字节，或持有 backing owner 的引用。缓存命中会获得该结果已验证的 tile 记录。取消只停止本次 Run 的工作。保留的 view 会继续持有存储所有者，并在最后一个引用退出前占用预算。
