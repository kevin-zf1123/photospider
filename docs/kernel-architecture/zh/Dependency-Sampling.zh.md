# 依赖采样算子

默认 registry 通过分阶段 Result program 执行 `image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter`。STMap 接收 canonical image Result 与 generic 坐标映射 Result；radius gather/scatter 接收 rank-one Float64 数据及 Int64 radius Result，并发布 generic Float64 tensor Result。

STMap program 为每个输出像素记录精确的 Data 和 Control support，并单独记录 Validation 闭包。图像输出会将所请求的通道 footprint 闭包为完整 RGBA 像素，因此 Result coverage 和 dirty mapping 都按像素观察。Radius continuation 为每个请求的输出样本记录精确 Data 或 Control support，并独立记录 `close_samples` Validation。

## 范围与执行路径

`OperationRegistry` 将 `image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter` 注册为 Result operations。每个定义提供 `start_result` 来创建 `ResultContinuation`，并提供 `specialize_metadata` 来检查绑定的 Result schemas 与 parameters、推导执行前的 output schema。Continuation poll 接收 `ResultProgramPhase`；Coordinator 履行 Result object、tensor 和 I/O Needs，callback 只能读取这些 Need 授权的 tensor samples。`ResultProgramPhase::report_numeric` 用于报告有界 `NumericDiagnostics`。C plugin 接口单独由 Result operation ABI 2 定义，也使用 Result ports，不提供 Value dependency executor。

Registry 持有不可变 operation definition。Continuation 保留 prepared metadata 和 resource owners；每次 poll 借用当前 phase 与获准的 input capabilities。Callback 将实际消费的 source ObjectIds 记录在 output association 中，并发布 Result。

## STMap Result 契约

`image.stmap(source, map, boundary)` 要求 source Result 使用 `photospider.image` v1 schema，含一个 `pixels` tensor 且没有 fields，语义为 canonical 线性预乘 Float32 RGBA，sample shape 为 `[F,L,Hs,Ws,4]`，`batch_axes=[F,L]`。Map 是一个恰含一个 tensor 且无 fields 的 Result，可使用任意结构有效的 schema id/version/member key，dtype 为 Float64，shape 为 `[Ho,Wo,2]`，facets 为空。其 `batch_axes` 可以为空，此时同一 map 广播到所有 source frame/layer；也可以等于 source batch axes。Source H/W 轴最大为 2^40。输出端口 `value` 是 canonical `photospider.image` v1/member `pixels` Result，Float32 shape `[F,L,Ho,Wo,4]`，并保留 source frame/layer extents。

必需 String 参数 `boundary` 没有默认值，Empty 请求也必须提供：

| 值 | 长度为 `N` 的轴上整数 tap `i` 的地址规则 |
| --- | --- |
| `constant` | 外部 tap 使用透明黑，不读取 source pixel。 |
| `clamp` | 限制到 `[0,N-1]`。 |
| `wrap` | 模 `N` 的非负余数。 |
| `reflect` | 周期 `2N`，重复端点：`p<N ? p : 2N-1-p`。 |
| `mirror` | 周期 `2N-2`，不重复端点：`p<N ? p : 2N-2-p`。 |

`N=1` 时，所有非 constant 模式都选坐标零。这些映射使用全局 source 坐标，不以 fragment 为单位分别应用边界。静态准备会检查 canonical source metadata、map tensor shape/facets/batch 兼容性、source 轴上限和 boundary 名称。Empty demand 执行这些检查，但不请求 sample payload。

对 map 坐标 `u` 和 `v`，操作从 `l=floor(u-0.5)`、`t=floor(v-0.5)` 确定四个邻居。小数偏移、权重和通道输出为：

$$
f_x = u-\tfrac12-l, \qquad f_y = v-\tfrac12-t, \qquad w_{dy,dx} = (dy ? f_y : 1-f_y)(dx ? f_x : 1-f_x),
$$
$$
y_c = \mathrm{Float32}\!\left(\sum_{dy=0}^{1}\sum_{dx=0}^{1} w_{dy,dx} \, s_{dy,dx,c}\right).
$$

上述公式中的每一步都是一次就近舍入的 Float64 运算：`f_x` 按 `(u-0.5)-l` 计算，每个权重是两个 Float64 因子的 Float64 乘积，每个乘积 `w*s` 先舍入再累加。因此权重是 Float64 值，而非精确有理数权重。取 `u=2^-56`、`v=0.5`、红色值为 `[1,-1]` 的 1x2 source 和 `wrap` 时，`u-0.5` 舍入为 `-0.5`，两个水平权重都变为 `0.5`，红色输出为 `+0`（Float32 位模式 `0x00000000`）。按精确有理数表达式计算会得到 `2^-55`。`image.stmap` 保留 Float64 结果，`test_dependency_sampling.cpp` 中的逐位 oracle 在四种调用方舍入模式下检查该结果。

### 执行阶段

非空请求在三次 continuation poll 内完成。Program 在第一个 Need 之前把请求的输出 footprint `Q` 闭合为完整 RGBA 像素；闭合后的 footprint `P` 就是 program 计算并发布的输出像素集合。

1. 第一次 poll 请求 source descriptor（role 8，Descriptor），以及 `P` 中每个 canonical box 对应的 map samples，roles 为 Control、Validation 和 Descriptor（14）。Broadcast map 的这些 map box 去掉 frame/layer 轴。`P` 之外的 map samples 不会被请求，因此未请求像素上的非有限 map 值不会被读取。
2. 第二次 poll 按行读取授权的 map 坐标：当 window 在 sample 轴上同时暴露两个分量时通过 `rectangle_run()` 读取，否则按分量调用 `row_run()`，因此接受 planar、tiled 和带符号 stride 的 map。存储方式取决于请求，见[map 坐标存储](#map-坐标存储)。它拒绝非有限或超出 `[-2^40,2^40]` 的坐标，为每个像素计算四个经过 boundary 映射的 tap，并构建下文所述的 Gather relation。随后它在 `P` 上投影该 relation，得到精确的 source Data footprint，并以 Data、Validation 和 Descriptor roles（13）请求该 footprint。当 `constant` 使所有请求像素的所有 tap 都落在 source 之外时，请求只含 Descriptor（8），不读取 source payload。
3. 第三次 poll 把每个授权的 source 像素复制一次到计入 Root 的缓存中，缓存含 `S` 个 RGBA Float32 值，`S` 为不同 source Data 像素的数量。随后它通过 `ResultBuilder::publish_tensor_kernel` 的 Footprint 重载一次发布整个 `P`，并 seal Result。

Empty demand 在第一次 poll 内完成：program seal 一个带 descriptor relation 的空 Result，不请求 map 或 source samples。因此 `OperationTiming::invocation_count` 对非空请求报告三次 poll，对 Empty demand 报告一次。下文发布阶段的 CPU tile callbacks 通过 `cpu_tile_callback_count` 单独报告。

### 依赖证据

发布的 tensor relation 是常数个 relation 节点的并集，节点数与 `P` 无关：

| 节点 | Support |
| --- | --- |
| 覆盖 `P` 的 `ResultRelation::gather` | 对每个输出像素，每个存在的 tap 记录一个完整 RGBA 像素的 source Data（role 1）。被索引的轴是 source 的 height 和 width；frame 和 layer 跟随输出的 frame 和 layer。Validation（role 4）闭合到 `max(1, atomic_trailing_axes)` 个尾部 source 轴，不扩大 Data。 |
| 覆盖 map 的 `ResultRelation::mapped` | 对每个输出像素，记录相同 height 和 width 处的 map 像素；map 带 batch 时还对应输出的 frame/layer。Map `atomic_trailing_axes <= 1` 时记录 Control 和 Validation（role 6）；分组更大时记录 Control（role 2），并用单独的 Validation 节点（role 4）闭合到这些尾部 map 轴。 |
| Descriptor relation | 两个输入端口的 Descriptor（role 8），通过 `bind_descriptor_relation` 绑定。 |

Map 节点建立在 `P` 的外接矩形上；发布时并集被限制到精确的 footprint `P`，因此该矩形内未请求的 map 像素不带 Control 边。Gather 表为每个输出像素存一行，每行含四个可选 tap：一个存在位，以及每个 tap 的 source y/x 坐标，坐标按 source height 和 width 轴所需的位宽打包。它是一张不可变表，而非每像素一个 relation 节点。`constant` 下位于 source 外的 tap 不存在，不贡献 support。

Tap 顺序固定为左上、右上、左下、右下。每个位于 source 内的 tap 都作为完整 RGBA 像素读取，包括权重为零的 tap；因此即使该 tap 权重为零且输出位不变，修改该 source tap 仍可能把输出标记为 potentially dirty。此类 tap 中的非有限分量仍会使 typed source validation 失败。Map Control witness 独立跟踪所选 map 像素的修改。地址从输出像素紧凑映射到 source 像素，因此稀疏请求不会展平或枚举巨大的 source 图像。

### 发布与并行算术

发布 callback 运行在 coordinator 线程上。它按行遍历每个 write window，计入 work，并创建最多 256 像素的 span；每个 span 记录全局输出坐标、两个 map 分量各自的指针和字节 stride，以及每个通道一个原始输出指针和字节 stride。Span 不跨越 map 行。每个输出像素根据 map 值和 source 缓存计算上述公式。Source box 多于一个时，tap 通过对 canonical source boxes 的二分查找定位到缓存位置。

当 `P >= 65,536` 且宿主提供 CPU staged tile 服务（`OperationTraits::cpu_staged_tiles`）时，coordinator 先收集全部 span，再对 span 索引提交一个 tile stage。Tile 大小为 `ceil(spans/64)`，因此无论 worker 数是多少，该 stage 最多有 64 个 tile callback；stage 请求 `min(4, maximum_workers)` 个 worker。Tile callback 只读取不可变的 map 值和 source 缓存，只写自己互不相交的输出 span，并在每个 span 前检查取消。Map 和 source Needs、window 服务、分配、work 计费和发布都留在 coordinator 上。所有 tile callback 在发布 callback 返回前完成。较小的请求或没有该服务的宿主在 coordinator 上直接计算每个 span。一个和四个 worker 得到相同的输出字节、Root work 和 tile 数。

在 AArch64 上，program 用 NEON 计算四个通道：把每个 tap 的 RGBA 值扩展为两对 Float64 lane，每对乘以 tap 权重后按 tap 顺序加到累加器，最后一次窄化为 Float32。其他目标使用等价的标量循环。两条路径的每个通道都从 `+0` 开始，乘法与加法分开执行，最后舍入一次到 Float32。`dependency_sampling.cpp` 使用 `-fno-fast-math -frounding-math -ffp-contract=off` 编译，因此编译器不会把乘法和加法收缩为融合运算，也不会重排求和。每次 poll 设置就近舍入和渐进下溢，并恢复调用方的浮点环境。Tile worker 运行在 CPU tile 服务的就近舍入环境下，见[并行执行模型](Parallel-Execution-Model.zh.md)。

### 代价

设 `P` 为闭合后的输出像素数，`S <= 4P` 为不同 source Data 像素数，`B_s` 和 `B_o` 分别为 source Data footprint 和 `P` 的 canonical box 数，`R_o` 为非空输出行数，`V` 为 typed Validation 读取的标量样本数。`G` 是规范化这些集合的精确 Footprint 运算代价。STMap program 的代价为

$$
T = O\!\left(P\,[1 + \log(B_s+1) + \chi \log(B_o+1)] + R_o \log(B_o+1) + S + V + G\right),
$$

其中变化的 tap 坐标不能装入一个 64 位排序键时 $\chi$ 为 1，否则为 0。Map 读取、tap 生成、Gather 表构建和插值对 `P` 线性。Source 缓存对 `S` 线性。每次 tap 查找要搜索 source boxes，每个输出行要在 `P` 中定位所属 box。对稠密矩形请求，`B_s` 和 `B_o` 很小，`V` 和 `G` 对 `P` 线性，因此整个 program 对 `P` 线性。高度碎片化的请求或碎片化的 source footprint 保留对数项，较大的 Validation 闭包至少花费 `V`。Gather 规范化在 tap 起点的外接框足够小时用位图标记这些起点，否则用稳定的字节 radix sort 排序，并跳过不变化的字节位置。稠密请求走位图路径，代价为 `O(P + A/64)`，其中外接框面积 `A <= 32P`。在 radix 路径上，变化的 source 坐标能装入一个 64 位键时排序最多八趟；否则每个变化的 source 轴各作为一个 64 位键，且每次读取由输出映射的 frame 或 layer 坐标都要搜索输出 boxes，这就是 $\chi$ 项。Gather 查询见[依赖数据](Dependency-Data.zh.md)。

Gather 表为每个输出像素存储 `4 * (ceil(log2 Hs) + ceil(log2 Ws))` 个坐标位和 4 个存在位，整张表按 64 位字向上取整；4096x4096 的 source 每像素需要 12.5 字节。已发布的 relation 在其生命周期内保留这张表。请求期间，program 还持有 map 坐标（见下文）和每个缓存 source 像素 16 字节。稠密位图在规范化期间占用 `ceil(A/64)` 个字。Radix 键和 scratch（每个存在 tap 各 8 字节）、span、window、Footprint 规范化 scratch 和输出 payload 另行计入 Root。

### map 坐标存储

请求满足以下全部条件时使用 map 行：至少 65,536 像素，闭合 footprint 是一个 box 且行宽至少 256 像素，map capability 来自单个 Result，请求的 map 区域覆盖完整 map 域。输出不必覆盖其完整域：在部分 frame 和 layer 上广播的 unbatched map 仍满足条件。Program 为每个输出行保留一个目录项。如果 map window 把整行暴露为一个 affine 或 planar run，该目录项保留这个 owning read window 及其原始指针和带符号 stride，不复制数据。否则它把该行复制到每像素 16 字节的 Root 分配缓冲区。同一请求中可以同时存在原始行和复制行。

其他请求把全部 map 坐标复制到计入 Root 的 `[P][2]` Float64 数组。两种情况下，program 在发布后销毁时释放这些 window 和缓冲区；已发布的 Gather relation 只保留 tap 坐标和存在位。保留的 read window 在请求运行期间使其读取的 map backing 保持存活，因此相对于数组的节省量取决于 map 的物理布局。

### 错误与资源

Program 的状态、tensor windows、workspace、输出和 work 都计入 Root limits。非法 map 坐标返回 `OperationFailed`；image/map metadata 格式错误返回 `TypeMismatch`；未知 boundary 返回 `InvalidArgument`。Typed image source validation 可能返回带 `FailureReason::InvalidDomain` 及其 source `input_id` 的 `InvalidArgument`。取消和资源耗尽保留各自的状态码；tile callback 把取消、资源耗尽和其他失败报告给 coordinator，coordinator 返回第一个记录的状态。失败的执行不返回部分发布的输出。

只有一个 canonical box 的闭合 footprint 使用 Region 发布路径：完整 spatial sample 字节几何可表示时，Result writer 使用 planar backing，否则为请求区域使用 affine backing。因此，从 `Ho=Wo=2^40` 的 map 发出的单像素稀疏请求在 Root limits 内写入四个输出样本，而不分配完整逻辑输出域。含多个 box 的 footprint 使用一块恰好覆盖这些 box 的 packed affine 分配。全域写入仍必须满足可用 Root 预算，同样的稀疏映射也使下文的巨大 source 用例不被枚举。

Root 容量限制请求规模。Gather 表、Footprint metadata 和 source 缓存计入 Host 与 Metadata 容量。使用默认容量时，较大的完整请求一旦使这些结构超过 Metadata 容量，就返回 `ResourceExhausted`；这类请求需要在执行资源配置中显式提高 Metadata、Host 和 Shared 容量。发布对所发布集合还保留局部 Footprint 上限：65,536 个 box 和 1,048,576 个 work 单位。

### 测试

`tests/integration/test_dependency_sampling.cpp` 覆盖五种 boundary、map frame/layer broadcast、三像素 identity map，以及从由 16 字节 payload 支撑的 2^40x2^40 broadcast source 稀疏读取。它还检查零权重 tap 仍是 dirty 证据、constant boundary tap 不请求 source payload，以及 Empty 跳过非有限 map。Source `atomic_trailing_axes=2` 时，Validation 闭合到完整 source 行，而 Data 仍只限于所选 tap。零权重 typed tap 中的非有限 RGB 分量以 `InvalidArgument` 使 source validation 失败；非有限 map 坐标先在操作中以 `OperationFailed` 失败。保留的 Result 在 context 退役后仍读到值 7，并保留两个 source 的关联。

`stmap_bitwise_oracle` 把每个输出通道的位模式与独立标量参考比较，覆盖 2x3、1x1 和 1x2 source、全部五种 boundary、`+/-2^40` 坐标、subnormal、有符号零和 `2^80` source 值，以及 1x2 的 planning tile。它在四种调用方舍入模式下检查 `2^100` 与 `-2^100` 的 tap 顺序折叠和上文的 `2^-56` 反例，并检查调用方模式被恢复。`stmap_sparse_dependencies` 检查精确的逐像素 Data dirty 映射、单个输出通道到其两个 source 像素的投影、稀疏请求中的空洞、请求之外的 NaN map 值，以及分组 source 和 map tensor 的 Data 与 Validation 分别闭合。`stmap_batches_and_strides` 覆盖 2x2 frame/layer source 上的 broadcast map 与 batched map，以及带符号 stride 的 nonspatial 和 spatial map backing。`typed_validation_work_batches` 检查[托管资源](Managed-Resources.zh.md)中所述的 256 样本 validation 计费批次。`stmap_map_rows` 在负 stride affine、负 stride planar、32x64 tiled 和部分拆分的 affine map 上运行 65,536 像素请求，使同一请求中同时出现原始行和复制行；它在各布局间逐位比较全部像素，并用标量 oracle 检查抽样像素。它还检查原始行与复制行分界两侧的精确 Data 投影和反向投影、两个 frame 与两个 layer 上的 broadcast map 和 batched map、调用方舍入模式的恢复，以及 Result 释放后保留的 relation 不持有 Payload 且 Metadata 少于 256 KiB。`stmap_cpu_tiles` 在全部五种 boundary 和向上的调用方舍入模式下，以一个和四个 worker 运行 256x256 请求，要求输出逐字节相同、work 相同、tile 数相同且不超过 64，并检查 oracle 值和精确 source support。

规模测试在显式提高的依赖 work 上限下运行 8x8 到 64x64 的 map。每个 map 坐标都是 0.5，因此在 `clamp` 下每个输出像素读取三像素行中的 source 像素 0 和 1。测试检查每个输出通道和精确 source support。修改 source 像素 1 使整个输出成为 potentially dirty，修改未使用的像素 2 则保持 clean。持有 Result 时 live Payload 增长小于 1 MiB；释放后回到基线。Root work 低于 `1600 * P`，`P` 每增加到四倍，work 增长小于五倍。稀疏规模测试在 16x16 到 64x64 的 map 上请求棋盘格单像素；它要求三次 poll、每个请求像素的 Root work 低于 `15000`，且尺寸每增加到四倍 work 增长小于六倍。这些上限只是这些工作负载的回归检查，1 MiB 增量也不是 context payload 上限、峰值内存上限或 RSS 声明。

取消在 50x50 map 上的两个位置测试。第一个位置在 callback body 结束后挂起 callback，取消 Run，要求返回 `Cancelled` 且 Payload 回到基线。第二个位置在一次 poll 内的正 phase work 计费处暂停 callback 线程并在此取消。该 work 计费必须返回 `Cancelled`，Run 必须报告 `Cancelled`，Payload 必须回到基线。Tile worker 内部的取消由通用 CPU tile 服务测试覆盖，没有 STMap 专用的故障注入。

端到端动态 demand 与 radius 测试位于 [`dependency_workflows/demand.cpp`](../../../tests/integration/dependency_workflows/demand.cpp)、[`dynamic.cpp`](../../../tests/integration/dependency_workflows/dynamic.cpp) 和 [`dependency_workflow_fixture.hpp`](../../../tests/support/dependency_workflow_fixture.hpp)。这些源码是当前行为覆盖入口。

## Result radius gather 与 scatter

`numeric.radius_gather(source, radius)` 和 `numeric.radius_scatter(source, radius)` 接收两个 Result。每个 Result 恰含一个 tensor 且没有 fields；两个 tensor 均为未分批 rank one，长度相同，`1 <= N <= 2^40`。Source dtype 为 Float64，radius dtype 为 Int64。输入 Result contract 接受的结构有效 schema id、version、member key 和 facets 均可保留。两个算子输出 `value` 端口为 `photospider.tensor` v1/member `samples` Result，Float64 shape `[N]`、facets 为空。

对输出索引 `o`，gather 使用 `radius[o]` 选择 source 索引；scatter 则对每个候选 `i` 使用 `radius[i]`：

$$
G(o) = \{i \in [0,N) : |i-o| \le r_o\}, \qquad S(o) = \{i \in [0,N) : |i-o| \le r_i\}.
$$

Gather 将 `G(o)` 化为裁剪后的半开区间。满足 `output < size` 且 `radius <= 2^40` 时，代码为：

```cpp
cursor = output > radius ? output - radius : 0;
end = output + 1 + std::min(radius, size - output - 1);
```

对每个请求的输出样本，Result program 以 Control、Validation、Descriptor（role 14）请求对应 radius 值，并同时请求另一输入的 Descriptor（role 8）。观测到 `[0,2^40]` 以外的 radius 返回 `OperationFailed`。Gather 随后以 Data、Validation、Descriptor（role 13）分块请求裁剪区间内的 source 值，每块最多 64 个样本，并按 source 索引升序累加。

Scatter 每块最多 64 个 radius 样本，扫描完整 radius tensor。它保留完整 Control witness，包括没有命中的候选，再仅对命中的 source 索引请求 Data。远端 radius 改变后，即使当前输出数值不变，也可能新增 source edge 并使输出 dirty；操作不需要逆向空间索引。

Dependency relation 将每个输出样本映射到精确的 source Data 或 radius Control support。`close_samples` Validation 单独以 role 4 记录。即使 Data 请求为空，两个输入的 Descriptor relation 仍存在，因此 typed validation 与数值支持相互独立。

每次 poll 向 Root 预留 32,768 字节 scratch。Continuation 通过 phase 计费的 work 扣减 Root。Coordinator 中的 Need 准入和 dependency projection 同时扣减 Root 与独立的 Run 级额度 `ExecutionOptions::maximum_dependency_work`（默认 1,048,576），因此一次 Run 累计的 Root work 不能与该额度直接比较。Continuation 使用 inline 数组保存至多 64 个候选索引，逐个处理请求 footprint 中的输出样本。每个和从正零开始，按 source 索引递增执行 Float64 左折叠；每次 poll 建立最近舍入和渐进下溢环境，然后恢复调用方浮点环境。非有限的命中 source、中间和溢出及非法的已观察 radius 返回 `OperationFailed`；typed Result validation 可使用 source `input_id` 和 `FailureReason::InvalidDomain` 返回 `InvalidArgument`。取消和资源耗尽保留各自状态码。

Empty demand 执行静态 metadata specialization，并返回不请求 sample payload 的空 Result。非空 demand 中，builder 逐点私有发布，全部请求样本完成后只 seal 一次。seal 前失败或取消不会返回部分 Result。输出 coverage 对应请求 footprint `Q`；只有调用者请求完整域时才生成完整输出。

## 执行状态与错误

```text
STMap Empty -> 静态 metadata/参数检查 -> 空 Result，无 payload Need
STMap 输出 Q -> 闭合为完整 RGBA 像素 P
  poll 1：Need(覆盖 P 的 map Control|Validation|Descriptor，source Descriptor)
  poll 2：读取并校验 map -> 每像素四个 tap -> Gather relation
          -> Need(投影 tap 上的 source Data|Validation|Descriptor，
                  或在 constant 下所有 tap 都在外部时仅 Descriptor)
  poll 3：缓存 source 像素 -> 一次发布 P
          -> P < 65,536 或无 tile 服务：coordinator 计算 span
          -> 否则：<= 64 个 tile callback，<= 4 个 worker，然后 join
          -> 提交 P 或回滚 -> seal Result

Empty 查询 -> 静态校验 -> 不调用 state 或读取 sample
非空 radius 查询 -> start -> Control Need -> 精确供给
  gather -> 每块至多 64 个 Data -> 有序折叠 -> 发布 sample
  scatter -> 分块扫描全部 Control -> Data 命中 -> 有序折叠 -> 发布 sample
```

Coordinator 在每轮 poll 提供由当前 Result Needs 授权的 object/tensor capabilities。Radius continuation 保存扫描游标、最多 64 个命中索引、阶段及 Float64 累加器。

## 校验、错误与行为证据

STMap 与 radius definitions 使用 `OperationDefinition::specialize_metadata` 执行静态 Result 校验。Registry 提供完整的 input Result metadata 和 parameters；specializer 校验声明的约束，并在执行前返回推导的 output schema。它不读取 tensor payload。`OperationRegistry::start_result` 随后校验已解析的 `ResultProgramQuery`，并调用 definition factory 创建 continuation。执行期间，`ResultContinuation::poll` 接收当前的 `ResultProgramPhase`，其中包含 callback 当前获准的 Result inputs 和 services；Coordinator 在各次 poll 之间履行 Needs。

`tests/integration/test_dependency_sampling.cpp` 检查有 seed 的 gather/scatter 结果、精确 Data/Control/Validation support、输出位不变但依赖关系变化的 radius edit、typed 与 opaque facets、命中非有限值、有序求和、Empty demand、取消和 Root 回滚。对 100,000 样本的 scatter，取消分别在 callback body 结束后和 radius Control 元素循环内的 phase work 计费处测试；两种情况都返回 `Cancelled`，Payload 回到 baseline。`N=2^40` 的 broadcast view 可用每个输入 8 字节的 backing gather 最后一个样本，无需扫描远端 Control 值。[dependency workflow demand scenario](../../../tests/integration/dependency_workflows/demand.cpp) 检查 Result-based 动态编辑、demand 替换和保留输出行为。STMap 覆盖见上文。

## 算法观察组与数值诊断

C++ `OperationOutputTraits::atomic_trailing_axes` 将完整尾轴分为同一原子观察，适用于 CPU staged Atomic 输出；0 保持逐标量观察。通用 shape `{N,C}` 设置 1 时观察 shape 为 `{N}`；axis `{3}` 设置 1 时观察 shape 为 `{1}`。宿主将部分样本请求闭包为完整元组，统一计算、验证和出具证书，再返回请求样本的交集。Result executor 将 STMap 输出观察闭包到完整 RGBA pixel。编译器边 metadata、structured bridge 和 joint 兼容性检查均保留分组，operation identity 包含该字段。Result operation ABI 2 的 `ps_result_tensor_spec_v2` descriptor 也暴露 `atomic_trailing_axes` 字段，与 batch rank 和 layout 字段并列。

Structured Result callback 通过 `ResultProgramPhase::report_numeric` 提交有界 `NumericDiagnostics`。报告包含实际 CPU profile、实现身份、evaluated values、strict fallback 和有界原因分类。Result callback dispatch 前，host 会从 Root 资源中准入对应 output/backend 的 `OperationTiming` 记录。每个 poll 的报告只合并一次，包括返回 Need 或失败的 poll。Callback 失败时，host 会先锁存首个错误，再合并该次报告；报告合并失败不会替换原始错误。若 `BackendUnavailable` 符合 CPU fallback 条件，host 会在 CPU 重启前记录失败的 GPU attempt。

Facilities 示例还会让 singleton 上游 Result callback 在报告一个 evaluated value 后耗尽其配置的 Host 余量，并返回 `OperationFailed`、`ShortIo`、Io origin 和 Group scope。其 C2 caller 已报告两个 values，因此 joint grouping 启用或关闭时，聚合值都是 3，且 upstream failure identity 保持不变。Timing record 在 callback dispatch 前已准入，因此 callback 用尽剩余 Host capacity 后，记录这些计数也不必扩展 Root-owned container。

`NumericDiagnostics::evaluated_values` 统计已报告的算术尝试，包括之后失败的工作。`OperationTiming::computed_elements` 是另一个计数：对 Result publication，它统计新增的 field rows 和 tensor samples。后续 Result revision 只统计相较前一 revision 新增的 rows 与 coverage；cache-hit publication 不增加 computed elements。逻辑计数超出 `UINT64_MAX` 或 sample cardinality 不可表示时，计数会饱和到 `UINT64_MAX` 并设置 `computed_elements_saturated`；恰好等于 `UINT64_MAX` 的精确计数不会设置该 flag。诊断计数饱和不会使原本合法的 Result 失效。这些诊断不改变语义身份，也不能证明精度界。见手动[数值 workflow](../../../examples/numeric_workflow/README.md)。

Structured execution 会按 producer output 和 backend 合并每个实际执行 observation 的报告。Cache hit 和未请求的 observation 不贡献算术计数。

## 限制与调用方处理

`image.stmap` 使用 canonical `photospider.image` source Result 和 generic Float64 map Result；三个已注册 key 均使用 Result 输入与输出。`ValueFragments` 保留为 atlas preparation/materialization 使用的 typed-backing helper，不是独立的 workflow 或 dependency-execution 入口。对 pure 且 cacheable 的 Result program，completed Result cache 可在 frozen 或 generation identity 改变时复用 sealed output，条件是 tensor footprint `Q` 完全相同、typed transitive source proof 匹配当前 bindings，且 Need 重放后的 ready facts 不变。Cache hit 会创建新 ObjectId 和当前 association，同时共享缓存的物理 backing；同一 frozen execution 内的重复请求仍由 weak same-ID producer sharing 提供。Cache 要求精确匹配 `Q`；重建当前 dependency evidence 时，可能重新运行已被 eviction 的 Whole ancestor。Incomplete source owners、已观察到的 backend fallback 和 `photospider.path_set` 均不参与缓存；可选 cache work 或 Root capacity 不足会导致 lookup miss 或跳过 retention。缓存的 ownership 与 proof 契约见[结构化 Result 与 tensor slots](Global-Results.zh.md)。Radius scan 或输出准入超限返回 `ResourceExhausted`；typed source 验证遵循 Result 输入验证契约，非法数值观察返回 `OperationFailed`，取消由 execution host 传播。
