# 当前实现与规格前置条件

2026-09-23 更新：package 0.20.0 [移除 13 个旧格式／颜色接口](../02-format-color/op_specs/FMT_legacy_retirement.md)。
下方历史交付表保留溯源；旧 typed 图像节点的注册不代表已迁移到 planar 执行。
新 FMT 规格保持 Proposed/未实现。


核对日期：2026-09-13。基线为本地 `ops@66b16339`；当前文档工作分支
`ops-specs@6e429e4b` 包含该基线，并额外实现 PNT-05A。项目版本为 **0.10.0**，
C operation ABI 为 **9**。来源为合并记录、当前注册代码、公开头文件和实现文档；
本轮仅更新文档，未重新构建或运行产品测试。以下链接提供可执行示例和既有验收入口。

## 合并记录与交付范围

| `ops` 合并提交 | 交付 | 当前读取入口 |
| --- | --- | --- |
| `2495393c` / PR #298 | 四 dtype、typed semantics/image v2、静态 shape、computed scalar、基础数值/颜色/连通域 | [Foundations](../../kernel-architecture/Foundations-Workflow.md) |
| `00864936` / basic CPU operators | 曲线、LUT、mask、滤镜、直方图、levels、字段生成等 21 项 CPU 算子 | [Basic operations](../../kernel-architecture/Basic-Operations.md) |
| `ffc5d0e2` / PR #301 | G4 精确依赖、按端口读取、STMap、动态 radius gather/scatter | [Dependency sampling](../../kernel-architecture/Dependency-Sampling.md) |
| `575a424d` / PR #314 | 命名多输出、独立 Region/依赖、可选 joint 执行；四个场景算子 | [Multi-output](../../kernel-architecture/Multi-Output-Operations.md) |
| `f7515f4f` / PR #324 | managed resources、分页全局结果、结构化表示、Layer/发光、atom 错误与质量；统计/FFT/连通域流程 | [Global results](../../kernel-architecture/Global-Results.md)、[资源](../../kernel-architecture/Managed-Resources.md) |
| `66b16339` / PR #325 | 直方图 factory 在读取源之前拒绝必需扫描阶段数已超限的尺寸/桶数 | [Integer statistics](../../kernel-architecture/Integer-Statistics.md) |

PNT-05A 的 `05f81347` 规格和 `6e429e4b` 实现属于当前 `ops-specs` 的附加提交，
不在上述 `ops` 合并基线中。旧 package 0.6/0.7、ABI 6/7 和“只有 8 项图像算子”
描述是早期快照，已由本页替换。

## 默认 registry 的可用节点

以下名称来自[注册入口](../../../src/lib/plugin/builtin_operations.cpp)和对应分类实现。
表格按能力归组；每项精确端口、参数、输入域和 Region 以链接的实现契约为准。
斜杠表示同前缀的多个 key，例如 `numeric.mean/variance` 表示 `numeric.mean` 和 `numeric.variance`。

| 能力 | 已实现 key | 实现契约 |
| --- | --- | --- |
| 数值与统计 | `numeric.add/subtract/multiply/divide`, `numeric.clamp`, `numeric.mean/variance`, `numeric.minimum/maximum/abs`, `numeric.ordered_scan` | [Numeric](../../kernel-architecture/Numeric-Operations.md)、[Basic](../../kernel-architecture/Basic-Operations.md)、[ordered scan 源码](../../../plugins/ops/01-numeric/numeric_ordered_scan.cpp) |
| 曲线与生成 | `numeric.sample_expression`, `lut.apply_1d`, `curve.sample_linear/sample_monotone`, `field.apply_lut_1d`, `field.coordinate`, `field.constant`, `field.smoothstep` | [Expression/LUT](../../kernel-architecture/Expression-and-LUT-Operations.md)、[Basic](../../kernel-architecture/Basic-Operations.md) |
| mask 与旧标签 | `mask.threshold/components/invert/combine/dilate/erode`, `component.count/area/bbox` | [Components](../../kernel-architecture/Component-Operations.md)、[Basic](../../kernel-architecture/Basic-Operations.md) |
| 滤镜与分析 | `field.box_mean/gaussian_blur/convolve/correlate`, `analysis.histogram`, `analysis.histogram_out_of_range`, `grade.levels` | [Basic](../../kernel-architecture/Basic-Operations.md)；`field.convolve` 已升级 staged regional，correlate/旧直方图等仍 Whole |
| 原有 RGBA/mask 链路与 mix | `image.exposure_gain/opacity/gaussian_blur/mask/source_over/downsample_box/brush_circle`, `mask.downsample_box`, `image.mix` | [Image](../../kernel-architecture/Image-Operations.md)、[Basic](../../kernel-architecture/Basic-Operations.md) |
| 动态依赖采样 | `image.stmap`, `numeric.radius_gather`, `numeric.radius_scatter` | [Dependency sampling](../../kernel-architecture/Dependency-Sampling.md)；STMap 是明确边界模式的 bilinear 子集 |
| 命名多输出 | `image.split_horizontal` → `full/left/right`；`image.convolve_channels` → `r/g/b`；`image.gaussian_blur_with_kernel` → `image/kernel` | [Multi-output](../../kernel-architecture/Multi-Output-Operations.md)；端口独立请求，joint 为可选 CPU 优化 |
| 当前分支局部修复 | `image.local_inpaint_navier_stokes_native_apple_silicon`；可选 `image.local_inpaint_navier_stokes_openCV` | [PNT-05A 实现](../09-composite/inpaint-ns-implementation.md)；Whole、opaque RGBA、binary mask，OpenCV adapter 需构建开关 |

原有 8 项有可选 Metal 后端；新增 CPU 算子、joint 与 structured Result 不因此获得 GPU 支持。
PNT-05A 仍有编译期 profile/shape 拒绝时机与规格不完全一致的已记录限制。

## 显式注册的全局流程与表示

下列 operation factory 返回可注册的 `OperationDefinition`。调用者冻结尺寸/schema 后使用
`OperationRegistry::register_operation` 注册，并配置
`ExecutionContextConfig::managed_resources`；这些 key 不由默认 registry 自动加入。

| 公开入口 | 可用计算链与输出 | 边界 / 示例 |
| --- | --- | --- |
| `make_statistics_operation` | `statistics.histogram` → `statistics.parameters` → `statistics.grade` | facet-free Int64 HW + UInt8 mask；分页稀疏整数频数、全局参数、Float64 pixels；[workflow](../../../examples/statistics_workflow/README.md) |
| `make_fft_operation` | `fft.forward_real`, `fft.import_response`, `fft.multiply`, `fft.inverse_real` | Float64 HW real、Full/R2CHalf Spectrum；逆变换输出 real projection + measured imaginary residual；[workflow](../../../examples/fft_workflow/README.md) |
| `make_component_operation` | `components4.labels` → `components4.area` → `components4.filter` | UInt8 HW，四连通 MinPixel ID，分页关联面积索引与 UInt8 mask；[workflow](../../../examples/components_workflow/README.md) |
| `make_layer_operation` | `layer.assemble/over/opacity/emit_front/emit_behind/flatten/response/response_over/coverage_raw_plus/raw_checked/raw_capped/weight/weighted_reduce/weighted_finalize/require_valid` | coverage 与 emission 分离；固定 linear-sRGB D65；[Layer runtime](../../kernel-architecture/Layer-Runtime.md)、[workflow](../../../examples/layer_workflow/README.md) |
| `*_schema` / `*_spec` | Spectrum、Bands、PathSet、DynamicPoints、Components、PlanarYCbCr、BrushState、IterativeState | 已实现描述、分页字段和校验；Haar/causal brush/diagonal iteration 有限定示例；[表示契约](../../kernel-architecture/Structured-Representations.md) |

表示和示例 helper 已交付的范围分别记录；通用 wavelet、路径布尔/描边、smudge、任意迭代 solver
仍需专用节点。`analysis.histogram` 与 `statistics.histogram`、`mask.components` 与
`components4.labels` 具有不同输入和结果契约，不能按名称互换。

## 当前契约与剩余边界

| 主题 | 已实现 | 仍需逐算子明确 |
| --- | --- | --- |
| dtype / shape | UInt8、Int64、Float32、Float64；Value rank 1..8、正轴长；静态输出、命名多输出；Result RuntimeCount 可为零 | 已有 Float32/64 HWC2 ComplexField，无原生 complex dtype；动态结果采用有版本 schema，不修改已发布 Value descriptor |
| 颜色 / alpha | image v2 支持明确模型/角色和 signed/HDR；CoverageRGBA 的 A∈[0,1]、A=0⇒P=0；Layer 独立 E | ICC/OCIO、任意空间转换和隐含颜色保存不自动提供；每个端口有自己的输入子集 |
| 依赖与发布 | protocol 1 精确 Value fragments；protocol 2 Result 分页、ObjectId/关联；CompleteBundle/StablePrefix/IndependentChunks | IndependentChunks 当前仅有序前缀子集；Conservative/Unknown 不能升级 Exact；终端 RequestRecord 不作可组合中间结果 |
| 预算与寿命 | root capacity/work/stages、mandatory backing、显式 I/O、cache-off 共享、最终 owner 释放 | `WithinBudgetOrFail` 只覆盖声明的 managed capacity；RSS/driver/OS cache 等排除，有限预算可能拒绝执行 |
| 错误与质量 | `Status.code/reason/detail`、Atom/ValidationDomain/Association/Group/Run/Waiter scope；`execute_atoms`；Measured/CertifiedBound | 普通 `execute` 仍 fail-fast；CertifiedBound 当前仅受限整数对角系统 factory，不覆盖任意 solver |
| 未交付功能族 | 可复用公共基础已具备 | 通用路径光栅、EDT/Canny、更多重采样核/地图生成、3D LUT、ICC/OCIO、RAW、时域、Deep、ML 等仍按各篇 Proposed 规格推进 |

资源、错误及数值边界详见[公共契约](contracts.md)。完整需求目录和 D1/D2/D3 成熟度
不代表所有条目已成为 registry 节点；后续执行链见[路线图](../14-roadmap/implementation.md)。
