# 内建图像算子需求与研究规格

2026-09-11 更新：G1/G2/G3/G5 的限定基础子集已通过 PR #298 交付。新增基础算子见[实现说明](../kernel-architecture/zh/Basic-Operations.zh.md)。C++ 算子按本目录的职责分类组织，每个算子一个源文件，见[源码目录](../../plugins/ops/README.md)。其余目录项仍为 Proposed。

本目录面向 Photospider 算子实现者、workflow 作者和使用这些能力的图像应用。目标是覆盖 Photoshop、Lightroom、Camera Raw、DaVinci Resolve、Clip Studio Paint、Nuke、After Effects 中主要的图像处理与分析能力，并拆解为可组合、可验证的计算接口。

状态为 **Proposed**。这是需求与算法研究，不是已接受的公共 API、实现完成记录或商业软件逐像素兼容承诺。既有英文 ADR 与 `docs/kernel-architecture/` 继续分别记录已接受决策与当前实现。研究检索日期为 2026-09-09；网页版本、手册版本和不可访问的材料在来源处注明。

默认采用明确的数学与颜色科学语义。商业软件兼容模式只有在版本、工作空间、位深、alpha 和结果证据充分时建立。首期集中于确定性的静态图像计算；时域、跟踪、机器学习、深度合成纳入依赖规划。

## 阅读入口

| 目录 / 文档 | 内容 |
| --- | --- |
| [规格模板](00-foundation/spec-template.md) | 每份规格必须回答的问题，完整程度与验收状态 |
| [综合研究报告](00-foundation/research-report.md) | 主要结论、设计取舍、专项核验与实施建议 |
| [候选依赖](00-foundation/dependencies.md) | 第三方库的职责、版本、许可标识和适配要求 |
| [当前实现与差距](00-foundation/current-state.md) | 8 个已注册图像/蒙版算子、现有类型/Region/ABI 限制 |
| [公共数据与执行约定](00-foundation/contracts.md) | 数组、图像、颜色、alpha、单位、Region、确定性、错误 |
| [数值基础](01-numeric/core.md) | 表达式、数组算术、统计、矩阵和工作流连接基础 |
| [曲线与 LUT 生成](01-numeric/curves.md) | 控制点、插值、采样、1D 与 3D LUT 的差别 |
| [格式与颜色管理](02-format-color/representation.md) | 通道、alpha、存储、模型、ICC/OCIO/HDR |
| [生成器](03-generation/generators.md) | 常量、坐标、渐变、噪声与测试图 |
| [路径](03-generation/paths.md) | Bézier、宽度曲线、采样与光栅化 |
| [蒙版与形态学](04-mask-morphology/masks.md) | 选区逻辑、距离场、扩缩、羽化、连通域 |
| [空间滤镜](05-filter/spatial.md) | 卷积、秩滤波、边缘、锐化、边缘保持 |
| [频域与修复](05-filter/frequency-restoration.md) | FFT、PSD 基础、反卷积、去噪、图像修复 |
| [散景与光学](06-optics/bokeh.md) | 固定/可变 PSF、CoC、深度、光谱、优化 |
| [调色](07-grade/adjustments.md) | 曲线、色阶、局部色调、选择性颜色映射 |
| [几何变换](08-transform/geometry.md) | 重采样、仿射、STMap、置换、液化 |
| [鱼眼与投影](08-transform/projections.md) | 投影模型、畸变校正及 3D 光栅化边界 |
| [混合与合成](09-composite/blending.md) | Porter-Duff、逐通道与非可分离混合 |
| [抠像、绘制与修补](09-composite/keying-paint.md) | Matte、despill、clone、heal、roto |
| [分析与示波器](10-analysis/scopes.md) | 直方图、波形、矢量、频谱、PSD、统计 |
| [RAW 与摄影流程](11-raw-photo/pipeline.md) | 传感器校正、去马赛克、HDR/堆栈/全景 |
| [时域、模型与特殊数据](12-temporal-ml/requirements.md) | 帧序列、光流、跟踪、推理、deep/AOV |
| [软件覆盖矩阵](13-coverage/software-matrix.md) | 七款产品的功能族映射与证据范围 |
| [原始需求映射](13-coverage/request-map.md) | 本轮每项需求对应位置、拆分、去重与缺口 |
| [执行路线](14-roadmap/implementation.md) | 前置契约、可执行阶段与验收关口 |
| [工作流验收](14-roadmap/workflows.md) | 可检查的端到端使用链路 |
| [来源与证据边界](15-references/sources.md) | 一手来源索引、私有材料与待验证问题 |

## 最重要的拆分

1. 数组维数由数据含义决定：独立 RGB 曲线为 `[N,3]`，三维颜色 LUT 为 `[Nr,Ng,Nb,3]`，二维矢量场为 `[H,W,2]`。路径先保存拓扑和控制点，再采样与光栅化。
2. 颜色模型、RGB 原色/白点、传递函数、场景/显示参照、存储类型、数值编码范围和 alpha 关联方式分别定义。一个“格式转换”菜单应组合多个基础算子。
3. 滤镜的单通道数学核心可以复用到多通道；颜色图像包装层负责线性化、alpha 和颜色空间语义。有符号导数与复数频谱需要通用数据表示。
4. 菜单功能可能是一个基础算子、一个复合 workflow、交互工具，或依赖模型/时域的数据系统。目录中的功能覆盖不能换算为相同数量的独立 C++ 算子。
5. 建议优先补通道组合、通用采样/重映射、mask/距离场、LUT 应用、统计归约、颜色管理和通用卷积。这些能力共同支持较多上层工具。

## 规格完整程度

`D1` 表示给出可实现的数学核心、主要参数与验收；`D2` 表示功能族与算法选择已界定，落地前仍须补专用接口或数据表示；`D3` 表示覆盖与依赖规划。D1 也不等于当前 ABI 已能承载，前置条件见每篇和公共契约。所有新算子名均为提议名，只有当前实现表中的名称可直接用于现有 registry。

文档中的“建议默认值”属于新设计。现有参数校验不自动填默认值，workflow 构造器须显式提交；本次未修改现有行为、运行算子测试、提交或发布代码。
