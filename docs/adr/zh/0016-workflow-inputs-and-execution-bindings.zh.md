# ADR 0016：将工作流输入绑定到每次执行

- 状态：Accepted

## 1. 核心摘要 (TL;DR)

工作流声明描述输入元数据，每次执行按名称提供不可变输入值或区域 source。编译器不把 payload 字节和地址放入 IR、plan 或 plan identity，因此一个 plan 可以使用独立输入快照运行。预检会在读取 source 前验证绑定名称和元数据，在算子 callback 前验证直接消费的标量 `Value`，并在每次区域读取后校验对应 source 样本。

## 2. 架构心智模型 (Mental Model & Intuition)

```text
WorkflowDocument                         每次 execute 调用
  声明 + 图                                  bindings
       |                                       |
       v                                       v
 analyze -> optimize -> plan            名称/元数据预检
                                  +------------+-----------+
                                  |            |           |
                                Value   RegionalSource  InputSnapshot
                                  |            |           |
                                  +------------+-----------+
                                               v
                                        callbacks -> 命名结果
```

声明像插座规格，固定名称及端口需要的逻辑类型。每次 `execute` 调用提供对应输入。`Value` 持有不可变字节；区域 source 同步填充宿主提供的请求缓冲区；`InputSnapshot` 提供不可变分块。并发调用可以共享 plan，同时保留各自的绑定快照。

## 3. 契约规约与接口 (Formal Contracts & APIs)

```cpp
struct PHOTOSPIDER_API WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  ValueDescriptor descriptor;
  Region region;
  StridedLayout layout;
  std::vector<ValueFacet> facets;
  std::optional<PlanarImageLayout> planar_layout = {};
};

using WorkflowInput =
    std::variant<WorkflowNodeOutput, WorkflowInputReference>;

struct PHOTOSPIDER_API ExecutionBinding final {
  std::string name;
  Value value;
  std::shared_ptr<const RegionalSource> source = {};
  std::shared_ptr<const InputSnapshot> snapshot = {};
  std::shared_ptr<const PlanarImage> image = {};
};
struct PHOTOSPIDER_API ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};

class PHOTOSPIDER_API ExecutionContext final {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

这些声明还原当前公开成员与字段形式，省略外围头文件和无关声明。

`WorkflowDocument` schema 3 保存输入声明、算子节点和命名输出。节点输入带标签，分别引用 producer 输出或输入声明；node id 与 input id 属于独立命名空间。声明使用唯一非零 id 和精确、区分大小写的可打印 ASCII 名称，长度为 1 至 128 字节，字符范围为 `0x21` 至 `0x7e` 且不含空格。每份文档最多声明 4096 个输入。每个声明必须且只能绑定一次，即使图中没有消费者也一样。

普通密集 `Value` 绑定必须匹配声明的元素类型、shape、完整逻辑 `Region`、零字节偏移、规范正向 row-major strides、精确字节数和封闭 facet 集。descriptor 的 rank 为 1 至 8，extent 均非零。字节数经过检查，必须为正且可由地址运算和宿主分配大小表示。facet 按 key 规范化，并按 key、版本和 payload 比较。一般 `Value` view 可以采用其他有效布局；工作流绑定采用更严格的密集声明契约。

区域 source 的 descriptor 和 facets 必须匹配声明；`read` 接收精确的非空逻辑 `Region` 和宿主拥有的可写字节，并报告实际填充的覆盖范围。指针在 `read` 返回时失效，source 必须支持并发读取且不能在读取期间变更。执行器会在使用成功读取的区域前校验其样本；因此较晚区域中的非法样本可能在更早区域已读取后才导致失败。snapshot 绑定是内核拥有的不可变分块存储；`read` 和内容身份访问受调用方指定的样本上限及协作取消控制。结构化图像声明和绑定使用 `PlanarImageLayout` 与 `PlanarImage`，其存储见 [ADR 0017](../0017-cpu-regional-execution-and-storage.md)。

`ExecutionBinding` 必须且只能选择一种表示。名称按多重集合校验，因此重复项不会在插入 map 时被静默覆盖。调用前会复制绑定及元数据；`Value` 和 image/snapshot 存储保留其 owner。Run 会将绑定快照保留到已准入工作退出。source callback 和输出 sink 的借用指针或 view 仅在各自调用规定的范围内有效。

编译期算子参数保留在 `WorkflowNode::parameters`。运行时标量是普通绑定 `Value`，并按算子端口约束校验。执行器会在 callback 前预检绑定名称和元数据，并验证直接消费的 `Float32Scalar` Value。`Float32Scalar` 端口要求密集 `Value`，该端口拒绝 `RegionalSource` 和 `InputSnapshot` 绑定。输入 payload 字节、指针和分配地址不参与 semantic、optimized、physical-plan 或 plan-cache identity；静态声明元数据、traits、图像布局和规范化输出需求参与身份。`InputSnapshot::content_identity` 对规范元数据和请求区域的精确样本位计算摘要，不包含分配、布局或分块几何。

## 4. 负面清单与边界 (Non-Goals & Explicit Boundaries)

- 声明不加载文件，也不持有调用方 payload。内核没有隐藏文件加载路径。
- 绑定不提供可选值或默认值，也不执行隐式类型转换、resize、布局重排或 facet 强制转换。
- 运行时值不会成为编译期参数或任意字面量节点。
- 输入到输出的直接透传不是工作流输出形式；命名输出选择算子节点的输出。
- `RegionalSource` 是同步内核 callback 契约，不是 provider ABI 或 codec 接口。
- 结构化 planar 图像使用独立的声明与绑定表示，不承诺样本以交错密集字节存储。

## 5. 后果与代价 (Consequences)

声明元数据或绑定名称格式错误、绑定缺失/多余/重复时以 `InvalidArgument` 失败；无法表示的密集字节尺寸运算以 `ResourceExhausted` 失败。声明类型、shape、region、layout、facets 或 source 元数据不匹配时以 `TypeMismatch` 失败。超出算子端口区间的标量，以及被图像域校验器拒绝的绑定图像样本以 `InvalidArgument` 失败。执行器在首个算子 callback 前检查名称、元数据、密集 Value 和直接消费的标量 Value；每次成功区域读取后、消费该区域前检查 source 样本。

同一个 plan 可以配合独立不可变绑定重复使用。调用方在 `execute` 复制期间不得修改绑定容器，并须自行同步 source 实现中可变状态。内核可能并发调用 source。取消以协作方式处理，并在有界 source/snapshot 工作和执行过程中观察。无效或过期 plan 会在检查绑定前被拒绝；有效入口后，取消优先于 stale 状态和普通执行失败。

调用方已有的输入保留不计入 `ExecutionContextConfig::maximum_live_bytes`。内核管理的输出、scratch、中间数据和传输分配会计入预算，并持续计账到最后一个 owner 释放。返回的不可变值可以晚于 execution context 释放。Input snapshot store 使用独立总字节预算；替换 snapshot 会创建新版本，旧版本仍可供已有读取者使用。
