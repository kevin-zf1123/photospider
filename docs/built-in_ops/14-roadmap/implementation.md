# 实施依赖与执行路线

2026-09-10：[ADR 0020](../../adr/0020-composable-operation-foundations.md) 已接受 G1/G2/G3/G5 的限定基础方案，总追踪 #287、顺序 #288–#297 已建立。仅该子集进入本轮开发；其余阶段继续 Proposed。交付为 ops-foundations→ops，不合 main。

状态 Proposed。顺序依据可复用性、现有代码约束和可检查工作流，不依据商业软件菜单数量。阶段是需求依赖规划，不是已创建的 Issue、实施授权或工时承诺。

## 优先要补的基础

1. 通用通道、scalar field、LUT 和 signed 数值表示，使计算结果能够真实连接。
2. 表达式/曲线采样、通用逐元素算术、mask 运算和统计，为调色、选区、分析共用。
3. 通用 resample/remap 与卷积核心，为变换、液化、景深和细节处理共用。
4. 明确的颜色管理与合成语义，为 HDR、广色域、Lab、LUT 和显示输出共用。
5. 全局归约、FFT、scan、动态数据与时间窗的执行边界，为分析、修复及后期流程提供可控实现。

这些基础比直接逐个复制软件界面更能缩短后续功能的实现链。此排序属于架构建议；库选择或公共类型变化仍需独立评审。

## 前置关口

| 关口 | 当前事实与必须作出的选择 | 首个验收流程 |
| --- | --- | --- |
| G1 数值表示 | 已接受保留4种 element，Float32/64计算，cast与range分离；complex显式实虚轴，不新增dtype | uint8→Float32→uint8 有界 round-trip；signed 梯度不丢负值 |
| G2 语义类型 | 新 image/scalar/vector/complex/profile 的契约与合法组合；本轮 image v2 替换 v1，无旧接口保留 | extract→process→merge；straight↔premul；Lab往返 |
| G3 shape/输出 | 已接受dtype与shape分离、静态轴+checked offset、bounded重复输入、单输出独立属性节点；变长path排除 | expression count→LUT→图像；mask→components和属性 |
| G4 空间依赖 | 任意变换、动态半径、全局scan/reduce；可先Whole，逐项增加必要映射 | identity STMap与跨tile warp等价；变半径脉冲 |
| G5 值组合 | 已接受computed Float32 {1}及兼容dimensionless单样本signal；消费前finite/range，静态shape与动态系数分离 | generator→运行时曝光gain；修改系数复用计划 |
| G6 外部输入 | 宿主负责 codec/profile/模型/帧序列加载及不可变快照，数据进入公开Value/区域源 | 解码图片→颜色转换→处理→宿主编码 |

G1..G5 不是要求一次重写内核。每个首批 workflow 先核验现有 generic 路径能否合法承载，无法承载的行为再形成最小契约变化。数值例子可以用当前 Float32/Float64 和固定 shape 起步。

## 分阶段可执行工作

| 阶段 | 功能切片 | 直接依赖 | 退出条件 |
| --- | --- | --- | --- |
| P0 接口试验 | 当前 8 op 现状核验；channel/scalar/LUT 最小垂直试验；确定新数据入口 | 现有主干，G1/G2/G3/G5 | 明确哪些新端口可用现有ABI，哪些需独立变化；保留8项行为 |
| P1 基础组合 | numeric arithmetic、expression、linear/monotone curves、channel extract/merge、cast/range、mask逻辑、threshold、基本stats | P0 | W1/W2/W3；每族至少一个公开入口样例 |
| P2 图像核心 | convolution/correlation、box/median/signed derivative、morphology/EDT、resample/remap/affine/resize、histogram/waveform | P1，G4 | W4/W5/W6；局部边界与Whole一致，无隐式gamma |
| P3 调色与合成 | transfer/matrix/Lab/ICC或OCIO适配、levels/curves/HSV-LCh、3D LUT、标准blend、mask局部调色 | P1/P2，G2/G6 | W7/W8；颜色/alpha/HDR规则可检查，兼容模式独立 |
| P4 专业静态 | FFT/PSD、guided/bilateral/deconvolution、bokeh、key/matte/heal、RAW、HDR merge、focus stack/panorama | 各自最小依赖，不要求全部P3 | W9/W10/W11；库版本与算法质量模式固定，global内存可控 |
| P5 时域与特殊 | frames/flow/tracking/stabilize/temporal denoise、ML推理、deep/AOV、3D投影光栅 | 序列/动态数据/外部模型契约，相关静态基础 | 输入时间/权重/资产可复现，质量指标明确，静态路径独立可用 |

可独立推进的例子：mask 基础无需等待 ICC；固定 PSF 可在现有 RGBA 表示上先实现，不必等待深度图遮挡；直方图可先Whole，不必等待通用分布式归约；源代码不存在自动完成的数据表示就不能写“直接接入”。

## 算子落地的验收关口

每个切片交付：名称/端口/shape/参数、CPU参考、独立解析或数值oracle、实际公开入口工作流、Region/资源/错误按风险验证、文档与实测命令。优化后端的容差、fallback、缓存和内存条件单列。新公共 ABI/API、ownership、持久格式、并发/内存边界使用 reviewed；普通现有接口算子使用 rapid。

库复用仅减少实现成本，不能替代契约和验收。选库前用真实图像和有界难例验证结果、支持 dtype/layout、线程/取消、临时空间、许可与安装消费。不要将一次 FFT 或 ICC 调用的时间当成完整 workflow 时间。

## 本轮已分配及其余待决内容

- G2 typed facet、端口和编译/缓存身份已在 ADR 0020 定稿，由 #289/#291 实现。
- G3 静态轴推断、单输出拆分节点、固定容量与有效计数已在 ADR 0020 定稿。
- G4 任意采样映射/全局scan的公共描述是否值得扩展，还是先保留Whole。
- 首工作空间已固定为 linear sRGB/Rec.709 D65，signed/HDR 合法；display 输出、ICC/OCIO 继续待后续专项。
- Lab 非可分离混合在超色域时默认哪种 gamut mapping，以及L*亮度/XYZ Y是否各设模式。
- mask soft Boolean 的默认产品语义、路径宽度自变量和 stroke overlap规则。
- RAW decoder 选择与处理状态、模型许可、视频时间基和deep表示。

这些问题已界定，可随第一条相关流程推进，不能把仍有关键空缺的D2/D3条目标成可以直接编码的完整规格。
