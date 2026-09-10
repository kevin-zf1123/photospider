# 当前实现与规格前置条件

2026-09-11 更新：`ops@2495393c` 已合并 foundations PR #298，#287/#288–#297 已交付。
当前实现使用 package 0.7 / ABI 7；以下旧基线与限制属于历史研究。
本轮基础算子研究见 [basic operations](basic-operations-research.md)。

2026-09-10 契约补充：执行基线为 `main@fba06270`、package 0.6.0/ABI 6。
以下表格记录原研究的 0.6 限制，不能用作新目标。[ADR 0020](../../adr/0020-composable-operation-foundations.md)
已接受 image v2 完整替换、四 dtype、静态输出及 computed scalar；#287/#288–#297
实现尚未交付，本次仅核查相关代码/文档，未重新运行产品测试。

本页根据 2026-09-09 工作区 `main`、HEAD `18d2126` 的源文件和实现文档读取整理。起始工作区无已有改动。本次仅研究与写文档，没有重新运行这些算子的测试；下面“已实现”是源代码事实，历史测试描述见链接。

## 已有图像与蒙版算子

| 当前 key | 已实现输入/参数与行为 | 需求归属 |
| --- | --- | --- |
| `image.exposure_gain` | RGBA + Float32 `{1}` gain [0,16]；RGB 乘 gain，alpha 原样 | 曝光的线性增益基础，EV 包装尚需定义 |
| `image.opacity` | RGBA + opacity [0,1]；RGBA 都乘 opacity | 图层不透明度 |
| `image.gaussian_blur` | radius Int64 [1,64]，sigma Float64 [0.1,64]；归一化可分离高斯、逻辑图像边界 clamp | 有现成 RGBA 高斯，通用 signed/单通道仍需扩展 |
| `image.mask` | RGBA + Float32 `[H,W]` mask [0,1]；RGBA 乘 mask | 应用蒙版 |
| `image.source_over` | 同尺寸前景/背景 premultiplied RGBA；Porter-Duff source-over | 标准合成 |
| `image.downsample_box` | factor Int64 [1,16]；ceil 降采样，边缘除实际样本数 | 整数 box 缩小 |
| `mask.downsample_box` | 同上，HW mask | 蒙版预览缩小 |
| `image.brush_circle` | image,x,y,radius,r,g,b,a；硬边圆点 source-over | 单次硬边盖印，不含连续笔迹/压感/抗锯齿 |

出处：[注册与参数](../../../plugins/ops/image_operations.cpp)、[C ABI 模块](../../../plugins/ops/rgba32f/image_plugin.c)、[完整图像语义](../../kernel-architecture/Image-Operations.md)。同一组 8 项已有可选 Metal 实现；CPU exact 为默认，MetalFp32 显式选择且具有数值 eligibility 与逐算子 CPU fallback，不能由此推断所有新算子已有 GPU 支持。[S4 使用说明](../../kernel-architecture/S4-Workflow.md)

## 现有数据和编译器限制

| 事实 | 对需求的影响 | 前置标记 |
| --- | --- | --- |
| `ElementType` 仅 UInt8、Int64、Float64、Float32；rank 1..8，各轴非零 | Int8/UInt16/Int16/Float16/complex 不是当前原生枚举；空路径/空列表需要显式表示策略 | G1 |
| 图像端口固定 `[H,W,4]`、`rgba;linear-srgb;premultiplied;hwc` | Lab、straight alpha、signed RGB、RGB-only 不得复用此 profile 欺骗校验 | G2 |
| RGB finite/nonnegative，alpha [0,1]，alpha=0 则 RGB=0 | 导数、负卷积、广色域变换、FFT 需通用数值表示或新语义 | G2 |
| mask 为无 facet 的 Float32 `[H,W]`，finite [0,1] | 深度、SDF、flow、标签图不能直接当 mask | G2 |
| shape rule 仅 Scalar/PreserveFirstInput/MatchAllInputs/Fixed/Shrink | 按运行时控制点数输出变长数组、通道变换、任意 resize 需逐项核验，Fixed 是注册描述中的固定 shape | G3 |
| 当前 operation 返回一个 Value | path 数据包、flow+confidence、计数+边界等是概念多输出；实现需拆算子/打包定义/明确扩展 | G3 |
| Region 仅 Whole/Elementwise/Halo/Shrink | 任意 warp、动态半径邻域、扫描线前缀、全局归约不能宣称有自动精确 ROI 推导 | G4 |
| generic Elementwise 要求输入与输出 shape 匹配，只有已知 scalar/mask 等有特例 | 图像局部 ROI + 整张小 LUT/卷积核/控制点表需要按端口区分 demand；暂用Whole或专门扩展 | G4 |
| bounded Float32Scalar 端口只接受直接 workflow input，scalar output 不支持该端口约束 | 数值 generator 输出连接现有 gain/opacity 不能假定已经合法；可组合标量需要专门解决 | G5 |
| 静态参数为 Int64/Float64/Bool/String，校验不填默认、不做强制转换 | 控制点/核/LUT 宜作为 Value 输入；建议默认由 workflow 构造端显式提供 | G5 |

出处：[Value](../../../include/photospider/data/value.hpp)、[OperationTraits/port/shape/Region](../../../include/photospider/plugin/operation_registry.hpp)、[operation ABI 6](../../../include/photospider/plugin/operation_plugin_api.h)、[Region 语义](../../kernel-architecture/Region-Semantics.md)。generic Value 可以保存有符号 Float32/Float64，但这不等于已有相应语义检查、输出推断与算子。

## 实施原则

先逐项验证能否使用现有 generic Value、固定输出或 Whole 实现，保留正确结果；将真正需要的公共变化归纳为最少的独立契约。不要为了算子目录的规模提前接受全新的通用张量运行时，也不要为了绕开 profile 校验把任意四通道数据标成 RGBA。

CPU 参考路径是每个新算法的验收基础。现有内核拥有执行、取消、资源与缓存；新算子必须申报临时空间并使用宿主分配，不另建 GPU 队列/任务系统。daemon 继续消费公共包，其职责不包括颜色算法和图像格式定义。边界参见 [ADR 0015](../../adr/0015-breaking-product-boundary-scope-reset.md) 及其后续修订。
