# 依赖采样算子

## 范围与执行路径

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

上述公式中的每一步都是一次就近舍入的 Float64 运算：`f_x` 按 `(u-0.5)-l` 计算，每个权重是两个 Float64 因子的 Float64 乘积，每个乘积 `w*s` 先舍入再累加。因此权重是 Float64 值，而非精确有理数权重。取 `u=2^-56`、`v=0.5`、红色值为 `[1,-1]` 的 1x2 source 和 `wrap` 时，`u-0.5` 舍入为 `-0.5`，两个水平权重都变为 `0.5`，红色输出为 `+0`（Float32 位模式 `0x00000000`）。按精确有理数表达式计算会得到 `2^-55`。`image.stmap` 保留 Float64 结果。修改算子时，应在四种调用方舍入模式下验证这个逐位验收场景，
并按测试规范现场编写、用后删除检查程序。

### 发布与并行算术

发布 callback 运行在 coordinator 线程上。它按行遍历每个 write window，计入 work，并创建最多 256 像素的 span；每个 span 记录全局输出坐标、两个 map 分量各自的指针和字节 stride，以及每个通道一个原始输出指针和字节 stride。Span 不跨越 map 行。每个输出像素根据 map 值和 source 缓存计算上述公式。

当 `P >= 65,536` 且宿主提供 CPU staged tile 服务（`OperationTraits::cpu_staged_tiles`）时，coordinator 先收集全部 span，再对 span 索引提交一个 tile stage。Tile 大小为 `ceil(spans/64)`，因此无论 worker 数是多少，该 stage 最多有 64 个 tile callback；stage 请求 `min(4, maximum_workers)` 个 worker。Tile callback 只读取不可变的 map 值和 source 缓存，只写自己互不相交的输出 span，并在每个 span 前检查取消。Map 和 source Needs、window 服务、分配、work 计费和发布都留在 coordinator 上。所有 tile callback 在发布 callback 返回前完成。较小的请求或没有该服务的宿主在 coordinator 上直接计算每个 span。一个和四个 worker 得到相同的输出字节、Root work 和 tile 数。

在 AArch64 上，program 用 NEON 计算四个通道：把每个 tap 的 RGBA 值扩展为两对 Float64 lane，每对乘以 tap 权重后按 tap 顺序加到累加器，最后一次窄化为 Float32。其他目标使用等价的标量循环。两条路径的每个通道都从 `+0` 开始，乘法与加法分开执行，最后舍入一次到 Float32。`stmap.cpp` 使用 `-fno-fast-math -frounding-math -ffp-contract=off` 编译，因此编译器不会把乘法和加法收缩为融合运算，也不会重排求和。每次 poll 设置就近舍入和渐进下溢，并恢复调用方的浮点环境。Tile worker 运行在 CPU tile 服务的就近舍入环境下，见[并行执行模型](Parallel-Execution-Model.zh.md)。

### map 坐标存储

请求满足以下全部条件时使用 map 行：至少 65,536 像素，闭合 footprint 是一个 box 且行宽至少 256 像素，map capability 来自单个 Result，请求的 map 区域覆盖完整 map 域。输出不必覆盖其完整域：在部分 frame 和 layer 上广播的 unbatched map 仍满足条件。Program 为每个输出行保留一个目录项。如果 map window 把整行暴露为一个 affine 或 planar run，该目录项保留这个 owning read window 及其原始指针和带符号 stride，不复制数据。否则它把该行复制到每像素 16 字节的 Root 分配缓冲区。同一请求中可以同时存在原始行和复制行。

### 错误与资源

Program 的状态、tensor windows、workspace、输出和 work 都计入 Root limits。非法 map 坐标返回 `OperationFailed`；image/map metadata 格式错误返回 `TypeMismatch`；未知 boundary 返回 `InvalidArgument`。Typed image source validation 可能返回带 `FailureReason::InvalidDomain` 及其 source `input_id` 的 `InvalidArgument`。取消和资源耗尽保留各自的状态码；tile callback 把取消、资源耗尽和其他失败报告给 coordinator，coordinator 返回第一个记录的状态。失败的执行不返回部分发布的输出。

## 算法观察组与数值诊断

Structured Result callback 通过 `ResultProgramPhase::report_numeric` 提交有界 `NumericDiagnostics`。报告包含实际 CPU profile、实现身份、evaluated values、strict fallback 和有界原因分类。Result callback dispatch 前，host 会从 Root 资源中准入对应 output/backend 的 `OperationTiming` 记录。每个 poll 的报告只合并一次，包括返回 Need 或失败的 poll。Callback 失败时，host 会先锁存首个错误，再合并该次报告；报告合并失败不会替换原始错误。若 `BackendUnavailable` 符合 CPU fallback 条件，host 会在 CPU 重启前记录失败的 GPU attempt。

`NumericDiagnostics::evaluated_values` 统计已报告的算术尝试，包括之后失败的工作。`OperationTiming::computed_elements` 是另一个计数：对 Result publication，它统计新增的 field rows 和 tensor samples。后续 Result revision 只统计相较前一 revision 新增的 rows 与 coverage；cache-hit publication 不增加 computed elements。逻辑计数超出 `UINT64_MAX` 或 sample cardinality 不可表示时，计数会饱和到 `UINT64_MAX` 并设置 `computed_elements_saturated`；恰好等于 `UINT64_MAX` 的精确计数不会设置该 flag。诊断计数饱和不会使原本合法的 Result 失效。这些诊断不改变语义身份，也不能证明精度界。
