# 实施依赖与执行路线

2026-09-13：按 `ops@66b16339` 和当前 `ops-specs@6e429e4b` 更新。
G1–G5 的限定基础、G4 依赖、多输出和 Phase A 已有实现，提交依据见
[当前状态](../00-foundation/current-state.md)。下列未交付切片保持 Proposed，
不构成新 Issue、实施授权或工时承诺。D1/D2/D3 表示文档成熟度，G 表示能力关口。

## 可复用的基础与剩余关口

| 关口 | 当前交付 | 继续实现时的边界 / 公开验收入口 |
| --- | --- | --- |
| G1 数值表示 | 四 dtype、cast/range、signed field；Spectrum complex pairs | UInt16/Float16/native complex 不在现有枚举；[Foundations](../../../examples/foundations_workflow)、[FFT](../../../examples/fft_workflow/README.md) |
| G2 语义类型 | image v2、通道/alpha、RGB/XYZ/Lab、ImagePlane、结构化 family、CoverageRGBA + emission | ICC/OCIO、更多颜色空间和通用导入仍需专项；[Layer](../../../examples/layer_workflow/README.md) |
| G3 shape/输出 | 静态轴推断、命名多输出、按输出 Region、Result schema/RuntimeCount/关联和空集合 | PathSet/Bands 等表示已具备，其全部变换/光栅节点尚未交付；[Multi-output](../../../examples/multi_output_workflow/README.md)、[表示](../../kernel-architecture/Structured-Representations.md) |
| G4 空间依赖 | protocol 1 精确 Value demand，STMap/radius gather/scatter；protocol 2 的 ResultRelation 与具名全局 recipe | 精确依赖须逐节点实现；Conservative/Unknown 保留保证等级；[G4 workflow](../../../examples/g4_workflow/README.md) |
| G5 值组合 | computed scalar、expression/LUT、全局 Result→参数→应用、多消费者和 owning backing | 静态参数/shape 与运行时数据分开；[统计 workflow](../../../examples/statistics_workflow/README.md) |
| G6 外部输入 | 现有 Value/RegionalSource/frozen input 入口，宿主拥有外部资产加载 | codec/profile/model/sequence 的具体 adapter、版本和语义继续待实现 |

跨关口执行基础已交付：root managed capacity/work/stages、mandatory temporary backing、
显式 I/O、共享结果生命周期、Atom/Association 等错误范围及质量证据。
这些契约都以 `WithinBudgetOrFail` 及各自支持子集为边界；新算法需要计算自己的预算，
不能由“有分页”推出任意尺寸完成保证。现有 schema 和 factory 已能支撑后续节点规格，
无需把所有新功能重新列为等待基础 ABI。

## 按真实缺口组织后续切片

| 阶段 | 已有可复用实现 | 尚未交付的主要切片 | 验收链与依赖 |
| --- | --- | --- | --- |
| P0 接口基础 | typed metadata、静态 shape、computed scalar、命名多输出、Result 与资源/错误 | 仅在具体新输入域或生命周期无法表达时提出最小共享契约变化 | 现有公开示例保持可用；不重新执行历史基础迁移 |
| P1 基础组合 | numeric、expression、linear/PCHIP、channel、mask Boolean/threshold、基本统计 | 更广 unary/broadcast/array 操作、噪声/shape generators、路径采样 | W1/W2/W3 已有子链；W4 需要 PathSet 消费节点 |
| P2 图像核心 | convolution、box/Gaussian、morphology、STMap、ordered scan、直方图与分页 CCL/area/filter | median/gradient/Canny、EDT/flood、更多 resampler/map generator/composition、scope render | W5/W6；继承 G4 和 Result，单列跨 tile 顺序/关联 |
| P3 调色与合成 | RGB/XYZ/Lab、levels、LUT/mix、source-over、Layer/emission/weighted/Response | ICC/OCIO/transfer/adaptation、HSV/LCh 等调整、3D LUT、更多 blend 和 group 语义 | W7/W8；颜色域、alpha、mass/weight 和 numerical failure 可检查 |
| P4 专业静态 | Full/Half FFT→response multiply→inverse；PNT-05A 在当前分支已实现 | window/PSD、Wiener/RL、保边/高级去噪、PSF/CoC/visibility、key/matte/Poisson、RAW/HDR/全景 | W9/W10/W11；逐项用现有 backing/iteration/schema，明确质量模式与外部资产 |
| P5 时域与特殊 | DynamicPoints/PathSet/BrushState/IterativeState 等承载子集 | frame/flow/tracking/temporal、ML、Deep/AOV、3D raster；完整笔刷反馈 | 仍需时间、外部模型/格式和各算法契约；静态流程独立可用 |

优先从一条完整使用链挑选缺失节点，例如 PathSet→stroke、signed field→gradient→Canny、
FFT→明确正则化→inverse。复用已有公开 factory 时给出注册和输入准备，避免引入
同名但不兼容的节点。FFT 的频域响应、连通域关联表、Layer emission 的语义均已有明确契约。

## 每个节点的执行链与验收

1. 在分类目录的 `op_specs/[code-index]_[function].md` 固定输入域、真实 key、输出端口/schema、
   参数和数学语义；未决设计保持 Proposed。模板见[规格模板](../00-foundation/spec-template.md)。
2. 指明复用哪种 Value/多输出/Result 接口，补完整 demand、dirty、returned mapping、
   finality、关联和缓存身份；算法专用索引不能假定由 schema 自动提供。
3. 计算 callback/scratch/输出/临时 backing/验证窗口及同时存活的 ancestry，给 work/stage
   和取消/失败条件。区分 managed peak 测量、容量准入和已证明的算法上界。
4. 通过公开 WorkflowDocument/Compiler/ExecutionContext 运行最小 workflow，提供独立 oracle、
   运行命令与预期结果，按影响验证错误域、预算、Region 或分页/寿命。仅报告实际运行结果。

使用现有接口的局部节点默认 rapid；公共 API/ABI、ownership、持久格式、并发/内存风险
使用 reviewed；release 仅显式要求。本次文档同步不创建开发任务或启动代码交付。

## 仍需专用决策的内容

- PathSet fill/stroke 的 AA、几何误差、宽度自变量与 overlap，以及索引和 dirty 范围。
- 更多采样核、缩小预滤波和负瓣 alpha 策略；Lab/LCh gamut mapping 和颜色管理资源。
- 迭代 solver 的成功/近似消费、停止条件、真实误差证据；有限 residual 不推导 CertifiedBound。
- RAW 处理状态、模型版本/许可、视频 timebase 与 Deep 合成的数据和算法契约。

已接受的 mask fuzzy/independent_coverage、四 dtype、image v2、multi-output、Result 与
Layer 不再列为未决问题；其支持范围以内直接复用，范围之外由首个相关 workflow 论证。
