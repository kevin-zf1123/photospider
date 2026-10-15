# ADR 0023：在固定 tile 之间记录依赖

- 状态：Accepted
- 英文权威文档：[ADR 0023](../0023-track-dependencies-between-fixed-tiles.md)
- 当前边界：0.34.0 中，原 §3.5 放入生产者语义 descriptor 的 facet 和资源身份已被取代；该 descriptor 是已解析的 Result schema 加上固定的全局事实。
- 相关：[ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)、[ADR 0017](0017-cpu-regional-execution-and-storage.zh.md)、[ADR 0018](0018-local-result-caches-and-frozen-execution.zh.md)、[ADR 0021](0021-independent-node-results.zh.md)、[ADR 0022](0022-freeze-operation-c-abi-v3.zh.md)

## 1. 核心摘要（TL;DR）

执行器目前为每个输出样本记录支撑它的输入样本，附带四种支撑角色和三种保证等级，并通过投影这份样本级 relation 回答 dirty、缓存和绑定替换查询。Photospider 将改为按固定的正方形 tile 计算和记录工作：整个 plan 使用同一个 tile 边长，划分每个空间 tensor 的空间轴；Tiled 输出每次完整计算一个 tile，Whole 输出由一个任务计算并一次发布其全部 tile；coordinator 记录每次计算请求过哪些生产者 tile 或哪些完整生产者。dirty 传播、已完成结果复用和绑定替换随后都基于这些 tile 记录，样本级 relation、它的角色和保证等级，以及支撑它们的精确集合运算都被删除。

## 2. 架构心智模型

tile 是发布、缓存和报告 dirty 的最小单位。plan 固定一个正方形 tile 边长；每个空间 tensor 按这个边长沿自己的高、宽轴切分，因此每个 tensor 有自己的 tile 数量和编号空间。每次计算记录它通过 Need 请求过的生产者 tile；这些记录在节点图旁边构成一张 tile 级的图。

下例是一个 STMap，其 `uv` 输入声明了空间布局，高、宽与输出相同。

```text
调用方请求 "warped" 的 tile (1,1)
        |
  warped: tile (1,1) --------------------------+
        |  poll 1：Need uv tile (1,1)          |
        v                                      |
  uv:     tile (1,1) 完成计算                  |  warped (1,1) 的记录：
        |  poll 2：Need source tile            |    uv      -> {(1,1)}
        v  （根据读到的 uv 值算出）            |    source  -> {(0,2), (1,3), (3,0)}
  source: tile (0,2) (1,3) (3,0) 完成计算 -----+
        |
        v
  warped (1,1) 计算其全部像素；载荷、覆盖范围和记录一起提交
```

同一份记录不读取像素即可回答三个问题：

- **dirty：** 编辑 source 的 tile (1,3) 时，标记所有记录中含有 (1,3) 的消费者 tile，再沿这些消费者的下游继续传播。
- **复用：** 一个已完成 tile 只要自身的物理身份及其记录中每个生产者的身份都未改变，就可以复用。
- **绑定替换：** coordinator 按 tile 比较新输入的内容；发生变化的输入 tile 作为 dirty 传播的起点。

## 3. 契约规约与接口

以下声明是提议的目标形态，不是当前代码。`PlanInput` 是 `photospider/compiler/compiler.hpp` 中现有的 `std::variant<PlanStepInput, PlanWorkflowInput>`。

```cpp
namespace ps {

// 整个 plan 共用的正方形 tile 边长：正的 2 的幂，包括 1。
struct TileGrid final {
  std::uint64_t tile_extent = 128;
};

enum class OperationRegionRule : std::uint32_t {
  Whole = 1U,  // 一个任务计算完整输出
  Tiled = 2U,  // 每个输出 tile 一个任务
};

// 单个 tensor 自身网格内的扁平 tile 编号：
// (batch_index * tile_rows + tile_row) * tile_columns + tile_column
using TileIndex = std::uint64_t;

// 一个生产者 tensor 的 tile 编号集合。具体表示由实现选择；
// 其存储和工作量计入 Root。
class TileSet;

struct TileDependency final {
  PlanInput producer;                 // plan step 的输出或 workflow 输入
  std::uint32_t producer_output = 0;  // workflow 输入不使用
  bool whole = false;  // 完整生产者：全部单元及其语义 descriptor
  TileSet tiles;       // Tiled 生产者中被请求的 tile；
                       // 为空且 whole 为 false：仅依赖语义 descriptor
};

// start 或 poll 回调提出的一个输入请求。
struct TileNeed final {
  std::uint32_t input = 0;       // 输入端口
  bool whole = false;            // 完整生产者
  bool descriptor_only = false;  // 只要语义 descriptor，不要载荷
  TileSet tiles;                 // 生产者自身网格中的 tile 编号；
                                 // 任一标志为 true 时为空
};

struct TileRecord final {
  std::uint32_t step = 0;
  std::uint32_t output = 0;
  TileIndex tile = 0;  // 单元编号：Tiled 输出的 tile，Whole 输出为 0
  std::vector<TileDependency> inputs;
};

}  // namespace ps
```

### 3.1 空间 tensor 与 tile 网格

已解析的 `ResultTensorLayout` 中 `spatial == true` 的 tensor 是空间 tensor。它的 tile 按 plan 的 `tile_extent` 划分其声明的 `height_axis` 和 `width_axis`。每个 batch 坐标各有自己的一组 tile，其他所有 cell 轴（包括通道轴）在每个 tile 中都完整覆盖。轴顺序取自布局；`{N,L,H,W,C}` 和 `{C,H,W}` 都是合法的示例。

对正的 tile 边长 `T` 和空间长度 `E`，该轴上的 tile 数为：

$$
C = \left\lceil \frac{E}{T} \right\rceil.
$$

```cpp
// 前提：tile_extent > 0。避免计算 extent + tile_extent - 1。
const std::uint64_t count = extent / tile_extent + (extent % tile_extent != 0);
```

边缘 tile 裁剪到空间边界。如果任一 tensor 的 tile 数超出 `TileIndex` 的范围，planner 拒绝该网格。

生产者是 plan step 的输出或 workflow 输入。一个 Result 恰好包含一个 tensor、该 tensor 是空间 tensor、且没有 field 时，它具有 tile 网格。

- 当算子声明 `Tiled`，且已解析的输出 Result 具有 tile 网格时，plan step 的输出为 Tiled。该规则依据已解析的输出 schema 决定，因此同一个算子可以对图像产生 Tiled 输出，对非空间 tensor 产生 Whole 输出。
- workflow 输入是顶层 `WorkflowDocument` 声明、由宿主在每次 Run 时绑定的输入。它没有算子；当其声明的 Result schema 具有 tile 网格时为 Tiled。该判定在编译期依据声明做出，与所绑定 Result 的存储方式无关。
- 其他生产者都是 Whole。

Tiled 生产者的单元是它的 tile。Whole 生产者恰好有一个逻辑单元，编号 0，它是计算、依赖记录、缓存和 dirty 传播的唯一键。Whole 输出内部的存储 tile 属于物理布局，不作为依赖键。

只有顶层文档声明 workflow 输入。authoring helper 以及以后加入的组合算子，都在 analyze 之前展开为普通节点；展开片段的输入变成连向为其供数的生产者的边，并遵循这些生产者的规则。

### 3.2 请求、计算与输入

调用方和算子都按 tile 编号指定工作；kernel 不做矩形到 tile 的换算。调用方请求某个具名 Tiled 输出的一个 `TileSet`，或请求任一具名输出的完整输出。对 Tiled 输出，每个被请求的 tile 由一个任务完整计算。对 Whole 输出，非空请求运行一个任务，计算完整输出并一次发布其全部 tile。空请求保留现有的短路规则。调用方可以从 plan 取得每个输出的网格：tile 边长、batch 数，以及每个空间 tensor 的 tile 行数和列数。

无论 Whole 还是 Tiled 任务，输入都只通过 Need 进入计算。coordinator 不预取输入，prepare 产生的静态活跃输入投影保持不变：投影之外的输入不会被请求或执行。一个任务可以在多次 poll 中提出 Need，并根据已读到的值选择后续 Need，STMap 和 LUT bake gate 都是这样做的。

一个 `TileNeed` 指定一个输入端口，以及以下三种请求之一：

- **tile：** 生产者自身网格中的一个非空 tile 编号集合，两个标志都为 false；
- **整体：** 完整生产者，`whole == true`、`descriptor_only == false`，`tiles` 为空；
- **仅 descriptor：** 只要语义 descriptor、不要载荷，`descriptor_only == true`、`whole == false`，`tiles` 为空。

其他任何组合都以 `InvalidArgument` 拒绝，包括两个标志都为 false 且没有 tile 的 Need。空 tile 集合从不表示整体请求或仅 descriptor 请求。对具有网格的输入，算子的 query 携带其网格，由算子自己把所需的样本坐标换算为 tile 编号；SDK helper 提供这项换算。对每个 Need，coordinator：

1. 拒绝超出生产者网格的 tile 编号，也拒绝对没有 tile 网格的生产者提出 tile 请求，这类生产者只接受整体或仅 descriptor 请求；
2. 把对具有 tile 网格的 Whole 生产者提出的 tile 请求归一为整体请求；
3. 调度其中尚未发布的生产者单元，等待期间不占用 worker；
4. 授予被请求单元的读取权限；整体请求授予完整生产者；
5. 把请求记入该端口的 `TileDependency`：Tiled 生产者记录被请求的 tile，整体请求记录 `whole == true`，仅 descriptor 请求记录 `whole == false` 的空集合。

一个任务的记录是该任务在当前绑定代内所有 Need 的并集。所有输入读取都经过 Need，所以记录天然完整，其中可能包含样本并未被计算用到的 tile。Whole 输出的存储 tile 共享其单元 0 的唯一记录。

### 3.3 校验范围

host 对授予的每个单元（完整 tile 或完整输出）检查 schema 和端口约束。算子在其计算的完整输出 tile 或 Whole 输出范围内施加自己的数值约束。只有 schema 或算子契约要求有限值时，非有限值才构成错误。

### 3.4 发布与存储

tile 的载荷、覆盖范围和依赖记录一起提交后，该 tile 才可见。校验、预算或取消失败时不安装部分 tile，也不安装记录。已发布的 tile 不可变。tile 可以以 view 的形式发布并保留更大的 backing owner；Root 计账以实际保留的 backing owner 为准，因此淘汰一个 tile 不一定释放一块 tile 大小的分配。Run 失败、取消以及已发布数据的生命周期遵循 ADR 0017。

### 3.5 语义 descriptor 与生产进度

生产者的语义 descriptor 包括其已解析的 Result schema，以及一经确定就固定的全局结果事实（例如 field 行数）；该 schema 固定 schema id 和版本、fields、tensor 元素类型、shape、空间布局拓扑和 domain。该 descriptor 描述结构；Result 不附着领域标签。operation preparation 按显式身份选定的资源进入消费方 step 的物理身份（§3.8），配置留在使用它的 step 上，不随数值 Result 传递。object 身份、发布 revision、当前覆盖范围和 sealed 标志记录的是生产进度。发布另一个 tile 只改变进度，不会使任何记录变为 dirty。语义 descriptor 变化时，引用该生产者的所有记录都变为 dirty。shape 或布局变化导致不再适配当前 plan 时，需要重新 prepare 和规划；旧 plan 的 tile 编号不会被复用。

### 3.6 记录的生命周期

- 重新计算成功后替换该单元的旧记录并更新反向索引，因此不再被读取的生产者 tile 不再向它传播 dirty。
- 重新计算失败时，上一代的 tile 和记录保持不变。
- 较旧绑定代的任务不能把自己的记录安装到较新的绑定代中。
- 已完成 tile 命中、checkpoint 命中或 block 命中时，要恢复被跳过计算的依赖。checkpoint 保存它所概括的前缀的 tile 和 Whole 依赖；无法重建这些依赖的 checkpoint，绑定其所依赖输入的完整身份。block key 保留现有对输入状态和内容的约束。

### 3.7 dirty 传播

coordinator 维护一个反向索引，从每个生产者单元指向引用它的记录。`whole == true` 的依赖匹配该生产者任一单元的变化及其语义 descriptor 的变化；仅依赖 descriptor 的依赖匹配语义 descriptor 的变化。dirty 查询从发生变化的 workflow 输入单元出发，每条可达记录只访问一次，为每个输出返回 dirty 单元：Tiled 输出返回 tile 编号，Whole 输出返回单元 0。

替换 Tiled workflow 输入的绑定时，按 tile 比较新内容，只把发生变化的 tile 作为起点；替换 Whole workflow 输入时，内容变化则以其单元 0 作为起点。实现绘画功能的宿主把绘制图层绑定为 Tiled workflow 输入，一笔只会让它触及的 tile 及其下游变为 dirty。dirty 传播阶段的工作量受变化输入可达的记录数和依赖数约束；绑定内容比较和内容身份计算另按实际检查的 tile 与逻辑数据量计账，两者均计入 Root 预算。

### 3.8 已完成 tile 的复用

已计算 tile 的身份由其 step 现有的物理身份（包括 tile 边长、已解析的输出 schema、实现、backend profile 以及影响结果的资源身份），加上单元编号和记录中每个生产者单元的身份组成。因此改变 tile 边长后永远不会复用旧 tile，没有输入的算子也一样。输入的内容身份按逻辑元素和语义描述计算；padding、stride、平面顺序和存储地址不参与。

## 4. 负面清单与边界

- 样本级依赖证据、支撑角色（Data、Control、Validation、Descriptor）和保证等级（Exact、Conservative、Unknown）不属于该模型。
- 发布、缓存和报告 dirty 的最小单位是 Tiled 输出的一个 tile，或完整的 Whole 输出。请求 tile 的一部分会计算整个 tile。
- 整个 plan 使用同一个正方形 tile 边长；算子不能声明自己的 tile 尺寸或形状。
- 非空间 tensor、带 field 的 Result 和含多个 tensor 的 Result 都是 Whole，只接受整体或仅 descriptor 的 Need；不存在按成员的 tile 请求。
- 组合或嵌套 workflow 不是 kernel 节点，在 analyze 之前展开为普通节点。
- kernel 不提供 Elementwise、Halo、Shrink 等静态区域规则。算子自行计算 Need；SDK helper 可以提供这些公式。
- coordinator 不把 Whole 计算拆成逐 tile 调用。PixelOE 等整图算法保留现有算法。
- 记录只描述一个 plan 和一代绑定，不是持久格式。

## 5. 后果与代价

**额外计算。** 只需要一小块区域的调用方要请求它涉及的每个 tile，并得到完整的 tile。如果一个消费者 tile 从许多生产者 tile 中各读几个样本，它要请求每一个这样的生产者 tile，每个都会被完整计算。

**错误范围扩大。** 由于 tile 被完整计算，在 schema 或算子要求有效值的前提下，tile 计算范围内任何位置的无效值都会让该 tile 及依赖它的请求失败。对 STMap 而言，被请求 tile 内某个未请求像素的 UV 不是有限值时，请求会失败。host 的 schema 检查覆盖授予的每个生产者 tile，包括没有被任何 tap 使用的样本。要求“同一 tile 内的稀疏请求绕开无效数据仍然成功”的测试不再描述现行契约。

**失效变粗。** 一次编辑会让请求过被编辑生产者 tile 任何部分的消费者 tile 及其全部下游变为 dirty。Whole 生产者（即使具有空间布局）或没有 tile 网格的输入（例如 generic 的 STMap map）的任何变化，都会使其消费者全部变为 dirty。编辑之后，Run 重新计算的量多于精确样本 relation 所需，已完成 tile 的复用命中率也相应下降。

**记录内存。** 记录的成本随其引用的生产者单元数量增长。覆盖生产者整个编号空间的稠密位集无论置位多少都占 $\lceil P/64 \rceil$ 个字：100 帧 7680×4320、tile 边长 128 时 `P = 204000`，每个这样的位集占 25,504 字节，若每个消费者 tile 有一条依赖，总量约 4.85 GiB。因此实现从排序去重的 tile 编号列表起步，并把其分配、复制和扫描计入 Root 的工作量和元数据预算；超出时返回 `ResourceExhausted`。

**tile 尺寸取舍。** tile 越小，任务、记录和调度操作越多；tile 越大，每次请求浪费的计算和每次编辑的 dirty 集合越大。在测量结果支持修改之前，保持当前默认边长 128。

**删除的机制。** 删除样本 relation 及其构造函数、packed Gather 证据、逐样本依赖证书、支撑角色与保证等级、作为依赖证据使用的精确集合规范化、作为依赖证据的 tuple 闭包、按查询划分 scope 的依赖 bundle 及其 rebind，以及按请求区分的 Actor。算子仍需要的结构与通道一致性检查保留为 schema 校验。多阶段 Need、静态输入投影、Root 资源准入、取消和回调退休保留。

**二进制接口。** Need 携带 tile 编号集合、整体生产者请求或仅 descriptor 请求；发布时不再携带 relation。算子 C 接口相应改变；其版本号与 ADR 0022 所述的缩减后接口一起确定。
