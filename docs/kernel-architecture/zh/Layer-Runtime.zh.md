# 已退休的 Layer、emission 与加权运行时

Package 0.19 在 schema 验证时以 TypeMismatch 拒绝下表六类旧 schema，以及含有
`photospider.layer` key 的 Result metadata。ResultBuilder 透传该拒绝，旧算子定义
不能注册为备选图像内存路径。下文记录原运行时，供迁移参考，不授权继续采用其数值
语义或执行命令。普通非图像 Result schema 保留。当前 planar 存储契约见
[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

英文权威文档：[Layer-Runtime.md](../Layer-Runtime.md)。本文件为对应中文说明。

本能力在 package 0.10 的 C++ API 中实现，C operation ABI 保持 9。CPU 算子使用
structured protocol 2 和资源预算管理的必需后备存储，不改变旧 image-v2 coverage
语义，也不提供 GPU 实现。

## 表示与观察

`layer_schema(kind, LayerSpec{H,W,1})` 创建版本一闭集 schema。working_space=1
固定为线性 sRGB primaries、D65 白点、scene reference、relative 单位，P/E 使用
同一空间。其他空间必须有受支持的模式，不进行隐式赋值、转换或 scene/display 互换。

| family | 记录与不变量 |
| --- | --- |
| `photospider.layer` | H*W 条 coverage Float32[4] 和 emission Float32[3]；P/A/E 有限；0<=A<=1；A=0 蕴含 P=0 |
| `photospider.layer_response` | H*W 条 Float32[4] Q/T；有限；0<=T<=1 |
| `photospider.raw_rgba_sum` | H*W 条 Float32[4] P/M；有限 M>=0；M=0 蕴含 P=0 |
| `photospider.layer_contributions` | 动态有序 Float64[8] P/A/E/w；前七项为合法 binary32 Layer 的精确提升；w 有限且非负 |
| `photospider.weighted_layer_sum` | 一条 Float64[8] Np/Na/Ne/W；有限 W>=0、0<=Na<=W；Na=0 蕴含 Np=0；W=0 蕴含全部分子为零 |
| `photospider.optional_layer` | 一条 UInt8 valid；coverage/emission 各有 0 或 1 条，与 valid 严格一致 |

Contributions、WeightedSum、OptionalLayer 使用集合域和单位置 working-space 规格。
Contributions 的 count 在运行时发现，可以为零；记录保存未加权样本，以允许未发布的
内部算术暂时出现随后相消的关联状态。该记录不属于已发布 WeightedSum。A=0、E 非零
仍是合法 Layer；总权重为零时没有 Layer 观察。

六类 schema 都要求 CompleteBundle。协调器在 observer、共享结果和下游访问之前检查
所有字段，coverage-only 消费者不能绕过 emission 的有限性检查。派生结果绑定并持有
按输入端口顺序排列的 Result ObjectId 与后备存储；旧 Layer 和新 Response、sum、
finalized Layer 分别保留各自所有权。静态检查有界且不做 I/O。显式
`validate_layer_result` 通过有计账的单记录窗口、根 work/I/O admission、取消令牌
及可选附加 work hook 执行验证。

## 数值契约

Layer 原语采用 binary32 nearest ties-to-even、渐进下溢、禁止 FMA，每个规定原语
分别舍入，返回时恢复调用者浮点环境。schema version 1 和各 canonical operation key
标识该算术。编译器未增加重结合或 Layer/Response 改写；不会自动规范化 signed zero。
P/E 允许有限的带符号与 HDR 数值。

Ts=round32(1-As)。有序 over 分别执行舍入后的乘法与加法，计算
`(Ps+Ts*Pb, As+Ts*Ab, Es+Ts*Eb)`。Coverage opacity 只缩放 P/A，保持 E。
Front emission 加 `gain*Eg`；behind 加 `Ts*(gain*Eg)`，各原语分别舍入并检查有限性。
Flatten 对显式 opaque background B 计算 `(P+E)+(1-A)*B`，输出 A=1。
Response 为 `Q=P+E,T=1-A`；Response over 为 `Qs+Ts*Qb,Ts*Tb`，Q/T 不能恢复
P/A/E。Raw plus 将 P/A 相加并生成非负、不限于 1 的 mass M。checked 转换要求 M<=1；
cap-alpha-keep-color 使用 A=min(M,1)，不除以 mass。

`layer.weighted_reduce` 先请求 complete contribution descriptor。对 [lo,hi)，使用
整数向下中点 mid=lo+(hi-lo)/2，先左子树、再右子树、再相加。单叶每项 w*component
分别按 binary64 舍入并复制 w。所有分量包括 Na/W 使用相同完整序号树，非 2 次幂长度
不补零。有计账的深度 64 栈与有界输入页保持归约树不受分页影响。空输入生成零 sum。
叶乘法和内部加法拒绝非有限结果，关联不变量只在最终 sum 发布前检查。
Finalize 先 binary64 除 W，再逐项舍入至 binary32，随后验证完整 Layer。W=0 不做除法，
返回 valid=false。公开 `weighted_layer_leaf/add` 返回可独立发布的合法 WeightedSum，
成功域比内部 scratch 窄；运行时内部归约不调用这两个 helper。

例如 w=2^-1074，P=+1/-1、A=.5 的两个叶子，其 Na 都舍入为零；完整 sum 为
Np=0、Na=0、W=2^-1073，合法成功。单叶 P=1 得到 Np=2^-1074、Na=0，则返回
AssociationUnderflow。类似地，binary32 的 P=1、A=2^-149 乘 opacity=.5 会失败，
不清零 P、不转为 emission。`Status.reason` 提供独立于诊断字符串的
AssociationUnderflow、ArithmeticOverflow、InvalidAssociation、EmptyWeightedResult。
失败不产生当前操作的部分结果；这里没有 CertifiedBound 或 RSS 保证。

Layer/Response 的成功域不同：P=FLT_MAX、A=.5、E=-FLT_MAX 的 Layer 与自身 over
会溢出，先 collapse 后的 Response over 可以成功。反之，P=E=FLT_MAX、A=1 是合法
Layer，但 collapse 会溢出。这些反例禁止未经证明的隐式代数替换。

## 公开算子与 workflow

`make_layer_operation(LayerOperation, LayerSpec)` 返回带有实际 CPU staged callback
的 `OperationDefinition`，可直接注册。固定 raster/space 写入输出 schema；同一 registry
注册不同尺寸变体时可选择不同 key。输入尺寸和空间在输入 I/O 前检查。
Builder 增长界来自实际输出：Layer 为 N 行、28N 字节，Response/RawSum 为
16N 字节，raster contributions 为 64N 字节；reduce 为单行 64 字节，finalize
最多 29 字节。它们不继承通用 builder 的百万行默认值。必需 disk、Host、work
和 stage 预算仍然生效。test_layer 单独验证大 raster 准入；另一个公开示例
photospider_layer_raster_workflow --large 完整执行 1920×1080 的 assemble → over →
weight，并检查全部字段。每批受 HW 当前行余量、单字段 I/O 窗口及 4096 字节输出
slab 工作集共同限制。算术仍按 row-major 顺序，仅合并传输和 append；整批像素
全部成功后才返回字段写入计划。canonical reduction tree 与 Whole flatten 输出分配
规则保持不变。

| key | 输入、参数、输出 |
| --- | --- |
| `layer.assemble` | canonical RGBA Value + 无 facet 的 Float32 HWC3，显式解释为 emission -> Layer |
| `layer.over` | front/back Layer -> Layer |
| `layer.opacity` | Layer；必填 Float64 factor∈[0,1]，舍入到 binary32 -> Layer |
| `layer.emit_front/emit_behind` | Layer + 显式 RGB emission；必填有限 Float64 factor∈±FLT_MAX，舍入到 binary32 -> Layer |
| `layer.flatten` | Layer + 显式 RGB opaque background -> Typed dense Whole RGBA Value |
| `layer.response/response_over` | Layer -> Response；两 Response -> Response |
| `layer.coverage_raw_plus` | 两 Layer，显式仅选择 coverage -> RawSum |
| `layer.raw_checked/raw_capped` | RawSum -> E=0 的 Layer，分别使用指定 mass 策略 |
| `layer.weight` | Layer raster；必填有限非负 Float64 weight -> 按 raster 序号保存未加权样本和 weight 的 contributions |
| `layer.weighted_reduce` | 动态 contribution collection -> 一个 WeightedSum |
| `layer.weighted_finalize` | WeightedSum -> OptionalLayer |
| `layer.require_valid` | OptionalLayer -> Layer 或 EmptyWeightedResult |

三通道 Value 端口要求无 facet，其解释由具名 working-space 契约显式给出，拒绝已有且
可能矛盾的元数据。Flatten 使用既有 Typed Image 端口和 image-v2 facet，要求 Whole
输出请求及足以容纳 dense 输出的预算；其他操作通过 Result 必需后备存储分页，不声称
任意图像 tiling 或全部效果能力。Host/Payload/Disk/I/O/work 限制可以导致有限的
ResourceExhausted，预算不承诺完成。
新增状态、页、witness 和有界适配 vector 都经过根 admission。旧 Footprint/Value
内部记账与平台分配保留 [Managed-Resources.zh.md](Managed-Resources.zh.md) 的排除项。
实测 managed capacity peak 不是进程 RSS 上界。依赖支持为 Conservative(All)，空集合
仍有 descriptor witness，不标 Exact。Result descriptor 使用独立 role=8 的 [0,1)
观察，不依赖数据行数；count/basis 改动按该 descriptor role 查询，不生成虚构数据行。

仓库运行命令：

```sh
cmake --build build/issue257-shared --target test_layer photospider_layer_workflow -j8
ctest --test-dir build/issue257-shared -R '^(test_layer|photospider_layer_workflow)$' --output-on-failure
```

实际安装 consumer：

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/layer_workflow -B out/phase-a-delivery/layer-consumer -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/layer-consumer -j8
out/phase-a-delivery/layer-consumer/photospider_layer_workflow
```

`examples/layer_workflow/main.cpp` 使用真实 RegionalSource、WorkflowDocument、Compiler、
ExecutionContext，并在 context 销毁后检查结果。Raster 的独立有理参考为
RGBA `[12.5,-2,1.25,1]`。count=3/5/7 的 `2^54,-2^54,1` 中点树例子预期 Np=0，
线性累加或补零可能得到 1；权重 `[2^53,1,1]` 预期 Np=Na=W=9007199254740994。
64/192/256 字节窗口保持参考一致。另验证空集合、零/负权重、严格下溢、scratch 相消、
空结果转图像失败、页/work 不足、cache-off 重复消费者共享和最终 pin 生命周期。
8192 条 contribution 共 512 KiB，在 256 KiB managed Host 和显式有限 100000-stage
预算下运行。`tests/unit/test_layer.cpp` 增加独立整数二进制有理数 over 参考、FMA/
重结合反例、mass 与 weight、signed zero 和浮点环境恢复。参考不依赖生产者分页，也
不把 Python 模型计账作为产品内存证据。
