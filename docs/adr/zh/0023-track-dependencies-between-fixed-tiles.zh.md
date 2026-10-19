# ADR 0023：在固定 tile 之间记录依赖

- 状态：Accepted
- 英文权威文档：[ADR 0023](../0023-track-dependencies-between-fixed-tiles.md)
- 当前边界：package 0.36.0 实现了 §3 所述的逐成员 tile 网格、成员局部区间 Need 和 Run 共享 stage。生产者的语义 descriptor 是其解析后的静态 Result schema；Result 不携带 facet 或附着的资源身份。
- 相关：[ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)、[ADR 0017](0017-cpu-regional-execution-and-storage.zh.md)、[ADR 0018](0018-local-result-caches.zh.md)、[ADR 0021](0021-independent-node-results.zh.md)、[ADR 0022](0022-freeze-operation-c-abi-v3.zh.md)

## 1. 核心摘要（TL;DR）

Photospider 按单个 Tensor 成员的固定 tile 计算和记录工作，而不是记录带支撑角色和保证等级的样本级依赖 relation。整个 plan 使用同一个 tile 边长，把每个 Result 的每个成员切分为各自的网格；每个被请求的成员 tile 由一个任务计算，coordinator 记录每次计算请求过哪些生产者成员 tile。dirty 传播、已完成结果复用和绑定替换都基于这些 tile 记录，样本级 relation、它的角色和保证等级，以及支撑它们的精确集合运算都被删除。

## 2. 架构心智模型

一个成员的 tile 是发布、缓存和报告 dirty 的最小单位。plan 固定一个正方形 tile 边长；每个成员按这个边长沿自己的切分轴切分，因此每个成员有自己的 tile 数量和编号空间。每次计算记录它通过 Need 请求过的生产者成员 tile；这些记录在节点图旁边构成一张 tile 级的图。

下例是一个 STMap，其 `uv` 输入声明了与输出高宽相同的空间布局。

```text
caller requests tile (1,1) of member 0 of "warped"
        |
  warped[0]: tile (1,1) -----------------------+
        |  poll 1: Need uv[0] tile (1,1)       |
        v                                      |
  uv[0]:  tile (1,1) computed                  |  record for warped[0] (1,1):
        |  poll 2: Need source[0] tiles        |    uv[0]     -> {(1,1)}
        v  (computed from the uv values)       |    source[0] -> {(0,2), (1,3), (3,0)}
  source[0]: tiles (0,2) (1,3) (3,0) computed -+
        |
        v
  warped[0] (1,1) computes all of its pixels; payload, coverage and record commit together
```

同一组记录无需读取像素即可回答三个问题：

- **Dirty：** 对 source tile (1,3) 的编辑标记所有记录中含 (1,3) 的消费者 tile，再沿这些消费者的下游继续传播。
- **复用：** 只要已完成 tile 的物理身份不变，且经其记录触及的每个 workflow 输入 tile 的内容不变，该 tile 就可复用。
- **绑定替换：** coordinator 逐 tile 比较新的输入内容；变化的输入 tile 作为 dirty 传播的起点。

## 3. 契约规约与接口

`PlanInput` 是 `photospider/compiler/compiler.hpp` 中的 `std::variant<PlanStepInput, PlanWorkflowInput>`。下列声明摘自 `photospider/data/tiles.hpp`、`photospider/execution/tile_dependencies.hpp` 与 `photospider/compiler/compiler.hpp`，省略了构造函数和成员函数。

```cpp
namespace ps {

struct PlanningOptions final {
  // ...
  std::map<std::string, TileRequest> output_requests;
  std::uint64_t tile_extent = 128;  // positive power of two, including 1
};

using TileIndex = std::uint64_t;
struct TileInterval final { TileIndex begin = 0, end = 0; };  // [begin,end)
class TileSet;  // normalized, Root-charged interval list

class TileGrid final {  // one per member
 public:
  static Result<TileGrid> from_tensor(const ResultTensorSpec& tensor,
                                      std::uint64_t tile_extent = 128);
  std::uint64_t tile_extent() const noexcept;
  std::uint64_t batch_count() const noexcept;
  std::uint64_t tile_rows() const noexcept;
  std::uint64_t tile_columns() const noexcept;
  TileIndex tile_count() const noexcept;
  Result<Region> region(TileIndex tile) const;
};

struct TileRequest final {  // member slot -> tiles
  std::map<std::uint32_t, TileSet> members;
};

struct TileNeed final {  // one input request from a start or poll callback
  std::uint32_t input = 0, tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;
};

struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0, tensor_slot = 0;
  TileIndex tile = 0;
};

struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0;  // unused for a workflow input
  std::uint32_t tensor_slot = 0;
  TileSet tiles;  // empty: semantic descriptor only
};

struct TileRecord final {
  TileUnit unit;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> shared;
};

}  // namespace ps
```

### 3.1 成员 tile 网格

每个 Result 的每个成员都有网格。`TileGrid::from_tensor()` 依据成员解析后的 `ResultTensorSpec` 和 plan 的 `tile_extent` 推导网格：

- **空间成员**（`ResultTensorLayout::spatial == true`）：tile 划分声明的 `height_axis` 与 `width_axis`。每个显式 batch 坐标有自己的 tile，其余每个 cell 轴（包括通道轴）在每个 tile 中完整覆盖。轴顺序取自布局；`{N,L,H,W,C}` 与 `{C,H,W}` 都是合法示例。
- **非空间成员：** 可分割轴是非 atomic 的 cell 轴。tile 划分其中最后两个轴；其余非 atomic cell 轴和全部显式 batch 轴作为 batch 坐标。只有一个可分割 cell 轴时，只沿该轴切分。没有可分割 cell 轴时，每个 batch 坐标形成一个 tile。没有 batch 轴的 scalar 或 atomic tuple 形成一个 tile。atomic 末尾轴在每个 tile 中完整覆盖。

对正的 tile 边长 `T` 与被切分的边长 `E`，该轴上的 tile 数为：

$$
C = \left\lceil \frac{E}{T} \right\rceil.
$$

```cpp
// Preconditions: tile_extent > 0. Avoids evaluating extent + tile_extent - 1.
const std::uint64_t count = extent / tile_extent + (extent % tile_extent != 0);
```

扁平 tile 编号为 `(batch_index * tile_rows + tile_row) * tile_columns + tile_column`。边缘 tile 被裁剪到成员边界。网格创建拒绝放不进 `TileIndex` 的 tile 数，但不要求成员的完整样本数放得进 `uint64_t`。

生产者是 plan step 的输出或 workflow 输入。workflow 输入是顶层 `WorkflowDocument` 声明、由宿主在每次 Run 中绑定的输入；它的网格在编译期依据声明的 Result schema 推导，与绑定的 Result 如何存储无关。plan step 输出的网格依据其解析后的输出 schema 推导。每个生产者的单元都是它的成员 tile；物理存储分区不是依赖键。

只有顶层文档声明 workflow 输入。编写辅助工具以及之后加入的复合 operation 在分析前展开为普通节点；被展开片段的输入变成指向为其供数的生产者的边，并遵循这些生产者的规则。

### 3.2 请求、计算与输入

调用方和 operation 以成员 slot 和 tile 区间指定工作。`PlanningOptions::output_requests` 把具名输出映射到 `TileRequest`；缺省的名称通过紧凑区间选择全部成员，空请求保持为空。未知名称、成员索引和超出网格的区间在发布前失败。每个被请求的成员 tile 由一个任务完整计算。每个成员的网格对调用方可从 plan 获得，对 callback 可从其 query 获得。

输入只通过 Need 进入计算。coordinator 不预取输入，preparation 产生的静态活动输入投影保持不变：投影之外的输入从不会被请求或执行。一个任务可以在多次 poll 中发出 Need，并依据已读取的值选择后续 Need，STMap 和 LUT bake gate 就是这样做的。

一个 `TileNeed` 指定一个输入端口和以下两种请求形式之一：

- **tiles：** `descriptor_only == false`，一个成员 `tensor_slot`，以及该成员网格中的一组区间。空集合不请求任何内容：coordinator 不创建依赖、不授予 descriptor，也不产生生产者工作。
- **仅 descriptor：** `descriptor_only == true`、`tensor_slot == 0` 且 `tiles` 为空。它授予语义 descriptor，不授予 payload。

operation 的 query 携带每个输入的每个成员网格，operation 自己把所需的样本坐标转换为 tile 区间；SDK 辅助函数提供这种转换。coordinator 在启动任何生产者工作之前校验完整的 Need envelope。对每个非空的 tile Need，它：

1. 拒绝超出生产者成员网格的区间；
2. 调度尚未发布的被请求生产者成员 tile，等待期间不占用 worker；
3. 授予被请求 tile 的读取权限（按 `atomic_trailing_axes` 闭合），同时授予生产者的 descriptor 事实；
4. 在对应生产者成员的 `TileDependency` 中记录该请求。

一个任务的记录是它在该任务和绑定 generation 内全部 Need 的并集；区间只在同一生产者成员内合并。每次输入读取都经过 Need，因此记录在构造上是完整的；它可能包含计算并未使用其样本的 tile。

需要完整输入的算法请求它读取的每个成员的每个 tile。一次遍历计算全部输出、或必须只产生一次外部可见副作用的算法声明 Run 共享 stage：一个静态 continuation，coordinator 对每个 Run、节点和 stage 至多实例化一次。消费方通过 `ResultSharedNeed` 请求 stage 已提交的成员 tile，每次 stage 提交都携带消费方继承的冻结 `TileEvidence`。继承的证据从不授予读取 stage 原始输入的权限。

### 3.3 校验范围

宿主对其授予的每个 tile 和发布的每个 tile 检查 schema 与端口约束。operation 对其计算的完整输出 tile 施加自身的数值约束。非有限值只在 schema 或 operation 契约要求有限值时才是错误。要求完整覆盖的校验器只在被请求并经 scope 闭合的覆盖完整时才看到完整覆盖；静态 publication scope 可以为可见性把成员分组，但不会扩大任何输入 Need。

### 3.4 发布与存储

tile 只有在其 payload、覆盖和依赖记录一起提交时才变为可见。校验、预算或取消失败不会安装部分 tile，也不会安装记录。已发布的 tile 不可变。静态 publication scope 可以推迟已完成 tile 的可见性：直到 `CompleteBundle` 完整，或直到 `StablePrefix` 成员中更早的 tile 已可见；单个 tile 完成本身不确立其成员的完整覆盖。tile 可以作为保留更大 backing owner 的 view 发布；Root 计费跟随实际保留的 backing owner，因此逐出一个 tile 不一定释放 tile 大小的分配。Run 失败、取消和已发布数据的生命周期遵循 ADR 0017。

### 3.5 语义 descriptor 与生产进度

生产者的语义 descriptor 是其解析后的静态 Result schema：schema id 与 version，1..16 个成员的元素类型、形状、空间布局拓扑、atomic 轴、domain 以及 publication scope。每个成员形状都在执行前固定，因此没有 descriptor 事实依赖产出的数据。descriptor 描述结构；Result 不携带附着的领域标签。operation preparation 按显式身份选择的资源进入消费 step 的物理身份（§3.8），配置留在使用它的 step 上，不随数值 Result 传递。对象身份、发布 revision、当前覆盖和 sealed 标志记录生产进度。发布另一个 tile 只改变进度，不使任何东西 dirty。语义 descriptor 的变化使所有指向该生产者的记录 dirty。不再适配 plan 的形状或布局变化需要重新 preparation 和规划；旧 plan 的 tile 编号不会复用。

### 3.6 记录的生命周期

- 成功的重新计算替换该单元先前的记录并更新反向索引，因此不再被读取的生产者 tile 不再向它传播 dirty。
- 失败的重新计算保持上一 generation 的 tile 和记录不变。
- 来自较旧绑定 generation 的任务不能把它的记录安装到较新的 generation 中。
- 已完成 tile 命中或 checkpoint 命中恢复其跳过的计算的依赖，包括继承的共享 stage 证据。block 命中只恢复 block 状态；当前 actor 的 Need 仍然有效。block 键保留其对输入状态与内容的现有约束。

### 3.7 dirty 传播

coordinator 维护从每个生产者成员 tile、以及每个共享证据节点到指向它的记录的反向索引。tile 依赖匹配其任一 tile 的变化以及生产者语义 descriptor 的变化；仅 descriptor 的依赖匹配语义 descriptor 的变化。dirty 查询从变化的 workflow 输入 tile 出发，每条可达记录只访问一次，并按输出以成员 slot 和 tile 编号返回 dirty 单元。

替换 workflow 输入的绑定时逐 tile 比较新内容，只以变化的 tile 作为起点。实现绘制的宿主把被绘制的图层绑定为 workflow 输入，因此一笔绘制只使其触及的 tile 及其下游 dirty。dirty 传播期间，工作量受从变化输入可达的记录和依赖约束。绑定内容比较和内容身份计算按其实际检查的 tile 和逻辑数据分别计费；两者都计入 Root 预算。

### 3.8 已完成 tile 的复用

已计算 tile 的身份由其 step 的现有物理身份（包含 tile 边长、解析后的输出 schema、实现、后端 profile 以及影响结果的资源身份），加上成员 slot、tile 编号和经其记录触及的每个 workflow 输入 tile 的内容组合而成。因此改变 tile 边长永远不会复用旧 tile，包括没有输入的 operation 的 tile。输入内容身份基于逻辑元素和语义描述计算；padding、stride、plane 顺序和存储地址不参与。

## 4. 负面清单与边界

- 样本级依赖证据、支撑角色（Data、Control、Validation、Descriptor）和保证等级（Exact、Conservative、Unknown）不属于该模型。
- 发布、缓存或报告 dirty 的最小单位是一个成员 tile。对 tile 一部分的请求会计算整个 tile。
- 内核执行没有整体 Result 模式，也没有代表完整 Result 的生产者单元。整体输出算法在 Run 共享 stage 或 SDK 适配器中运行，其消费方仍请求成员 tile。
- 整个 plan 使用同一个正方形 tile 边长；operation 不能声明自己的 tile 大小或形状。
- Result 没有 field、运行期成员数或动态 tile End；每个成员的网格在编译期固定。
- 复合或嵌套 workflow 不是内核节点；它们在分析前展开为普通节点。
- 内核不提供 Elementwise、Halo 或 Shrink 这类静态区域规则。operation 自行计算其 Need；SDK 辅助函数可以提供这些公式。
- 记录描述一个 plan 和一个绑定 generation；它们不是持久格式。

## 5. 后果与代价

**额外计算。** 只需要一小块区域的调用方要请求它触及的每个 tile，并得到完整的 tile。从许多生产者 tile 各读几个样本的消费者 tile 要请求其中每个生产者 tile，而每个都被完整计算。需要完整输入的算法在计算前请求全部输入 tile，Run 共享 stage 在 Run 剩余时间内持有其完整结果。

**更宽的错误域。** 由于 tile 被完整计算，在 schema 或 operation 要求合法值的地方，tile 计算中任何位置的非法值都会使该 tile 及需要它的请求失败。对 STMap，被请求 tile 内某个未被请求像素的非有限 UV 值会使请求失败。宿主的 schema 检查覆盖每个被授予的生产者 tile，包括没有采样点使用的样本。

**更粗的失效。** 一次编辑使所有请求过被编辑生产者 tile 任何部分的消费者 tile 及其全部下游 tile dirty。读取了完整输入的 Run 共享 stage 的消费方，会因该输入的任何变化而 dirty。编辑后 Run 重算的内容多于精确样本 relation 所需，已完成 tile 的复用也相应更常未命中。

**记录内存。** 记录的开销随其包含的区间边界数增长，而不是随 tile 数增长。覆盖生产者整个编号空间的稠密 bitset 不论置位多少都要 $\lceil P/64 \rceil$ 个字：对 100 帧 7680×4320、128 像素 tile，`P = 204000`，每个这样的 bitset 占 25,504 字节，若每个消费者 tile 一个依赖，总量约 4.85 GiB。因此 `TileSet` 存储规范化的半开区间，并把其分配、复制和扫描计入 Root 的 work 与 metadata 预算；超出时返回 `ResourceExhausted`。共享 stage 证据每次提交只存一次并被引用，因此 Q 个 tile 消费 P 个 stage tile 不会产生 P×Q 份副本。

**tile 大小的权衡。** 更小的 tile 增加任务、记录和调度操作的数量；更大的 tile 增加每次请求浪费的计算和每次编辑的 dirty 集大小。默认边长 128 保持不变，直到测量结果证明需要调整。

**删除的机制。** 样本级 relation 及其构造器、packed Gather 证据、逐样本依赖证书、支撑角色和保证等级、用作依赖证据的精确集合规范化、用作依赖证据的元组闭包、query 作用域的依赖 bundle 及其重绑定、按请求键控的 actor、整体 Result 执行、whole Need 以及 Result field 都被删除。operation 仍需要的结构与通道一致性检查保留为 schema 校验。多阶段 Need、静态输入投影、Root 资源准入、取消和 callback 退役保留。

**二进制接口。** Need 携带成员 slot 与半开 tile 区间，或仅 descriptor 请求；发布不携带 relation。Result operation C ABI 6 承载这些记录。冻结的前缀协商接口仍是 ADR 0022 的独立目标。
