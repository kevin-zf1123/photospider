# ADR 0021：路由命名结果并联合已就绪的 Atomic 工作

- 状态：Accepted
- 英文权威文档：[ADR 0021](../0021-independent-node-results.md)

## 1. 核心摘要（TL;DR）

一个 operation node 可以产生多个命名结果，每个结果都有自己的元数据、依赖、observation 契约和错误路径。Compiler 和 executor 用 `ValueRef` 标识每个结果，因此未使用的 sibling 不会为已选结果增加工作。可选的 CPU joint session 能一同推进兼容且已就绪的 Atomic observation，同时为每个成员保留独立结果。

## 2. 架构心智模型（Mental Model & Intuition）

Compiler 根据静态输入元数据和参数解析所有输出 descriptor 与 facets。执行只沿着有需求的结果端口及其相关输入边推进。启用 joint 执行后，协调器可组合来自同一 operation 实例和 snapshot 的就绪成员；每个成员仍拥有自己的读取、状态、证书、缓存身份和终态。

```text
                  一个 operation node
                  /       |         \
             output A  output B   output C
                 |         |          |
             demand A  demand B     未选择
                 |         |       不执行
              Atomic    Atomic
                 \         /
             已就绪的 CPU joint poll
                /                 \
            Need A                B outcome
              |               /             \
       解析 A 的输入       success B       error B
              |                |              |
          再次 poll         发布 B       只失败 B 的消费者
```

`ValueRef` 是 `(node_id, output_index)`。它独立于物理 PlanStep 索引路由结果。`RequestRecord` 是完整查询的终端 observation，不是样本级依赖。`Atomic` observation 表示限制请求范围后仍稳定的值或错误，通常对应一个 generic 样本或一个完整图像像素。

## 3. 契约规约与接口（Formal Contracts & APIs）

```cpp
struct SemanticOutput {
  std::string key;
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  bool effective_atomic = true;
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
  ObservationKind observation_kind = ObservationKind::Atomic;
  FailureDelivery failure_delivery = FailureDelivery::RequestFailureOnly;
  std::uint32_t atomic_trailing_axes = 0;
};

struct OperationTraits {
  std::vector<OperationOutputTraits> outputs;
  std::uint32_t joint_contract = 0;
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

这些 C++ 声明展示结果路由字段，省略了外围类型和校验。`OperationTraits` 以声明顺序保存输出。当前记录版本是 20；当前 package 为 0.28.0，C operation ABI 为 11。每个输出都有唯一名称及独立推断的 descriptor 和 facet 集。C operation descriptor 采用相同的有序模型，最多包含 64 项。多输出 operation 必须确定且无副作用。单输出 operation 明确声明 `value`。

Workflow 边按名称选择精确的 producer 端口。调用方可见的 `WorkflowOutput` 选择 node 与 port，再为结果指定唯一标签。Compiler 将输出名解析为声明顺序索引，并用 `ValueRef` 记录结果。静态推断使用完整输入元数据；输出的 `input_indices` 投影决定其可执行输入祖先和 callback 视图。投影后的 C callback 输入仍携带原始 schema 索引。

Compiler 根据输出声明的 observation kind 和相关输入祖先计算 effective observation kind。只有被选祖先也都是 Atomic，结果才保持 Atomic。`atomic_trailing_axes` 可将 generic 值尾部的完整维度组成一个 observation tuple；图像像素保留所有逻辑通道。Consumer 的 Atomic 要求只检查被选结果及相关祖先，因此无关 sibling 不会改变该结果的合法性。

每个有需求的结果由一个单输出 `PlanStep` 表示。未选择的纯输出不会增加执行需求。因此，同一个 node 在调用方请求多个输出时可以对应多个 PlanStep，每个步骤都携带该结果自己的契约和身份。C++ invocation 与 C output sink 会收到声明顺序的 `output_index`。

`FailureDelivery::RequestFailureOnly` 将失败作为该 observation 的整体错误返回，不允许把多个 Atomic observation 批量执行。`FailureDelivery::PerAtomOutcome` 只用于实现完整结果协议的 staged Atomic operation。Joint poll 对每个输入成员返回一个 `Need`、成功值或成员级错误。缺失、重复或未知的成员结果都属于协议错误。公开 `execute` 对完整的请求输出集合仍返回一个聚合成功或错误。

`OperationTraits::joint_contract` 是可选的 CPU 执行能力；仍需提供 singleton dependency callback。Contract 1 联合来自同一 node 的不同 Atomic 输出。Contract 2 联合不同 `AtomKey` observation，可包含同一输出的多个坐标。协调器只组合共享 operation、静态参数、输入 snapshot 和 CPU backend 的已知就绪需求，不等待未来请求，也不跨 Run 合并。`ExecutionOptions::enable_joint` 控制分组，默认启用。

每个 joint 成员有独立 waiter 和终态。成员级错误只影响该成员和实际依赖它的消费者。协调器为整个组计量一份 continuation 和 scratch reservation，并分别计入各成员的 adapter、输出和 workspace；同一 backing owner 只计量一次。每次 poll 返回时，该 poll 借出的 member service 失效。Contract 1 遇到可恢复的 joint 准入或执行错误时，会先释放联合 reservation，再用 singleton callback 执行未完成成员。协议错误、取消、stale 和 group/run 级错误会作为失败返回。Contract 2 中，整个 group 的错误是终态，因为所有成员属于一个坐标批次。

内置示例 operation `image.split_horizontal` 声明 `full`、`left`、`right` 三个输出。它需要 image 和一个严格位于输入宽度内部的整数 `split_x`。`full` 保留完整 shape；`left` 的宽度为 `split_x`；`right` 的宽度为 `input_width - split_x`。每个输出保留 image facet，并只请求对应源坐标。Callback 可以发布一个由请求源 fragment 支持的 owner-backed view。当前示例源码说明 typed image 绑定仍待 planar migration，因此它用于说明 API，不构成安装后的 planar workflow 可运行证据。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 多输出不意味着共享求值、共享缓存键或通过一次 callback 发布多个 `Value`。每个被请求结果遵循自己的执行契约。
- 选择一个结果时，不会仅因同一 node 有纯 sibling 而执行该 sibling。带副作用的 operation 仍按声明契约保留为 singleton root。
- Fetch union 不能替代每个输出的 Data、Control、Validation、Descriptor 关联及其证书。
- Joint 执行是可选的物理优化，不保证只 poll 一次、成员并行执行或独立成员共享同一错误。
- Joint 分组只适用于兼容的 staged Atomic CPU 契约，不组合 `RequestRecord` 或不同 node。
- 动态输出数量、运行后添加输出端口和自动通道裁剪不属于固定的声明顺序输出表。

## 5. 后果与代价（Consequences）

输出元数据和结果身份包含被选输出契约、解析后的 descriptor 与 facets、静态参数及实际输入依赖。改变输出角色、shape、相关输入投影或 observation 契约会改变其身份。无关 node id 和整张图的身份不能替代被选输出的内容依赖。

每个发布结果都拥有自己的字节，或持有 backing owner 的引用。缓存命中、flight join 和 joint 成员都会获得结果级的已验证证据。取消一个 waiter 不会取消其他仍活跃的 waiter；所有成员都不再有活跃 waiter 时，共享工作才停止。保留的 view 会继续持有存储所有者，并在最后一个引用退出前占用预算。

Joint 执行增加成员状态和共享状态；其峰值内存可能高于逐个执行输出。Reservation 或准入无法满足时，协调器会在契约允许的情况下退回 singleton 执行。调用方可以关闭分组，查看 `joint_groups`、`joint_polls` 和 `joint_fallbacks`，并为保留的命名输出留出预算。结构协议错误、取消和 stale 工作会明确返回，不会作为独立计算重试。
