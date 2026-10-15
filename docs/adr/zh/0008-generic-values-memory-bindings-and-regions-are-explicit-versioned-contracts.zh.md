# ADR 0008：Value 与输入绑定显式声明内存布局

- 状态：已接受
- 当前边界：原决策中的语义 facet、Value 资源绑定和多种输入绑定形式已被结构化 Result 边界取代。下文陈述当前契约；参见 [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、[结构化 Result 与 tensor slot](../../kernel-architecture/zh/Global-Results.zh.md) 和 [Tensor 描述](../../kernel-architecture/zh/Tensor-Semantic-Metadata.zh.md)。

## 1. 核心摘要 (TL;DR)
`Value` 显式描述逻辑类型、形状、有效覆盖范围和字节布局，只持有自身的存储 owner。Workflow 声明以固定的 Result schema 定义必需输入，每次 Run 对每项声明提供且只提供一个匹配的 Result 绑定。指针或 payload 字节不会暗中决定 graph 含义。

## 2. 架构心智模型

```text
WorkflowInputDeclaration (id, name, result_schema)
                    |
                    +-- Run binding: ResultRef (Value or PlanarImage backing)
                    |
                    +-- schema check + authorized tensor Need --> operation
```

例如 RGBA tensor 的形状为 `[height, width, 4]` 时，channel 轴可保持完整，planner 同时请求一个矩形空间区域。逻辑 Region 坐标描述元素，stride 描述地址。Planar image 保留分离的 channel plane，不会被描述为交错存储。

## 3. 契约规约与接口

```cpp
struct ValueDescriptor { ElementType element_type; std::vector<std::uint64_t> shape; };
struct StridedLayout { std::uint64_t byte_offset; std::vector<std::int64_t> byte_strides; std::vector<std::uint64_t> origin; };
class Value {
 public:
  static Result<Value> create(ValueDescriptor descriptor, Region region,
                              StridedLayout layout,
                              std::vector<std::uint8_t> bytes);
};
struct WorkflowInputDeclaration { std::uint64_t id; std::string name; std::shared_ptr<const SchemaTemplate> result_schema; };
struct ExecutionBinding { std::string name; ResultRef result; };
```

当前元素类型包括 `UInt8`、`Int8`、`UInt16`、`Int16`、`Int64`、`Float32` 和 `Float64`。形状 rank 为 1..8，每个 extent 均非零。`Region` 是 rank 匹配的逻辑覆盖范围，以半开区间表达。布局使用带符号 byte stride 和 origin；校验会检查完整寻址范围是否落在持有的 storage 内，包括负 stride 和零 stride。Value 字节使用不可变共享 storage。

Value 保存 descriptor、Region、layout 和共享的不可变 storage owner。颜色、采样和 LUT 的解释是消费它的 operation 的显式静态参数。Profile 等不可变 operation 资源通过传给 compiler 的 `ResourceSet` 进入 operation preparation，由 prepared operation、plan 和执行状态持有。

通用 Value 可以使用空 Region，但某些 operation port 要求非空覆盖。详见 [Region 语义](../../kernel-architecture/Region-Semantics.md)。

每次 Run 必须为每项输入声明绑定一次，包括未使用的声明。每项声明都携带完整的 `result_schema`；缺少 schema 时编译返回 `InvalidArgument`。绑定的 `ResultRef` 必须在 callback 运行前匹配该结构 schema。复制绑定期间调用方容器不得修改；Result storage 保持共享的不可变所有权。Payload 字节属于运行数据，不参与 compiler identity。

## 4. 非目标与明确边界
- 逻辑 Region 不是字节范围，也不表示物理连续。
- 通用 Value 不代表持久化格式或序列化协议。
- Value 和 Result 不携带语义 facet 或附加的领域描述，也不持有 operation 资源。
- `Value` 和 `PlanarImage` 提供 backing storage，不是另一种 workflow 输入形式。
- Planar storage 不是交错 RGBA storage。系统不会从裸指针推断可写外部 producer binding。

## 5. 后果与代价
Rank、shape、边界或布局错误会在 Value 发布前失败。绑定的 schema 不一致会在 callback 开始前被拒绝。越界、算术溢出和有界 payload 耗尽会按具体操作返回类型化错误或分配异常。调用方须通过 compiler 的 `ResourceSet` 提供 operation 资源，并遵守借用 callback 缓冲区的有效期。
