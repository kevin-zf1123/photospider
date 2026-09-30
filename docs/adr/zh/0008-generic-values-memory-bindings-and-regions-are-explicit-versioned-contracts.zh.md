# ADR 0008：Value 与输入绑定显式声明内存布局

- 状态：已接受

## 1. 核心摘要 (TL;DR)
`Value` 显式描述逻辑类型、形状、有效覆盖范围、字节布局、语义 facet 和保留资源。Workflow 声明定义必需输入，每次 Run 对每项声明提供且只提供一个匹配绑定。指针或 payload 字节不会暗中决定 graph 含义。

## 2. 架构心智模型

```text
WorkflowInputDeclaration (type, shape, region, layout, facets)
                    |
                    +-- Run binding: Value | RegionalSource | InputSnapshot | PlanarImage
                    |
                    +-- validated read or exact demand --> operation
```

例如 RGBA tensor 的形状为 `[height, width, 4]` 时，channel 轴可保持完整，planner 同时请求一个矩形空间区域。逻辑 Region 坐标描述元素，stride 描述地址。Planar image 保留分离的 channel plane，不会被描述为交错存储。

## 3. 契约规约与接口

```cpp
struct ValueDescriptor { ElementType element_type; std::vector<std::uint64_t> shape; };
struct StridedLayout { std::uint64_t byte_offset; std::vector<std::int64_t> byte_strides; std::vector<std::uint64_t> origin; };
class Value {
 public:
  static Result<Value> create(ValueDescriptor, Region, StridedLayout,
      std::vector<std::uint8_t>, std::vector<ValueFacet> = {}, ResourceBindings = {});
};
struct WorkflowInputDeclaration { std::uint64_t id; std::string name; ValueDescriptor descriptor; Region region; StridedLayout layout; };
struct ExecutionBinding { std::string name; Value value; /* alternatively source, snapshot, or image */ };
```

当前元素类型包括 `UInt8`、`Int8`、`UInt16`、`Int16`、`Int64`、`Float32` 和 `Float64`。形状 rank 为 1..8，每个 extent 均非零。`Region` 是 rank 匹配的逻辑覆盖范围，以半开区间表达。布局使用带符号 byte stride 和 origin；校验会检查完整寻址范围是否落在持有的 storage 内，包括负 stride 和零 stride。Value 字节使用不可变共享 storage。

每个 Value 最多有 64 个 facet。每个 key 唯一且为不超过 256 字节的可打印 ASCII，host 会在发布前按 key 排序。每个 facet payload 不超过 64 KiB；聚合 payload 也受上限校验。资源绑定保留明确的 owner。

通用 Value 可以使用空 Region，但某些 operation port 要求非空覆盖。详见 [Region 语义](../../kernel-architecture/Region-Semantics.md)。

每次 Run 必须为每项输入声明绑定一次，包括未使用的声明。绑定须匹配声明元数据和要求的完整覆盖范围；输入来源形式必须且只能选择一种。复制绑定期间调用方容器不得修改。不可变 source 状态必须支持并发读取。Payload 字节属于运行数据，不参与 compiler identity。

## 4. 非目标与明确边界
- 逻辑 Region 不是字节范围，也不表示物理连续。
- 通用 Value 不代表持久化格式或序列化协议。
- Planar storage 不是交错 RGBA storage。系统不会从裸指针推断可写外部 producer binding。

## 5. 后果与代价
Rank、shape、边界、facet 或布局错误会在 Value 发布前失败。绑定的 shape、layout 或 facet 不一致会在 callback 开始前被拒绝。越界、算术溢出和有界 payload 耗尽会按具体操作返回类型化错误或分配异常。调用方须保留显式资源 owner，并遵守借用 callback 缓冲区的有效期。
