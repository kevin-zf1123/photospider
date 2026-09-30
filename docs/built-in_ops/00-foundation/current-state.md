# 当前实现与规格前置条件

Photospider 0.28.0 提供 operation C ABI v11。本文概括当前包内可核对的注册入口、数据类型和执行边界；单个算子的端口、参数、数值行为和 Region 规则以对应实现契约为准。

## 默认 registry 与类型边界

默认 registry 的注册入口位于[`builtin_operations.cpp`](../../../src/lib/plugin/builtin_operations.cpp)，公开 operation C ABI 定义于[`operation_plugin_api.h`](../../../include/photospider/plugin/operation_plugin_api.h)。当前分类指南描述的是各族已实现子集；逐 key 的参数、dtype、shape、Region 与错误契约仍以对应专题为准。

存在 key 只说明 registry 接受该定义，不单独证明任意存储路径都可执行。`OperationTraits::planar_storage_capable` 和结构化 callback 决定 operation 能否进入 planar 执行路径；[`operation_registry.cpp`](../../../src/lib/plugin/operation_registry.cpp) 实施该门控。

[`ElementType` in `value.hpp`](../../../include/photospider/data/value.hpp) 定义七种内建元素类型：`UInt8`、`Int8`、`UInt16`、`Int16`、`Int64`、`Float32` 和 `Float64`。descriptor 的 rank 为 1 到 8，各轴长度必须大于零。浮点 `Value` 可保留通用位模式；算子的数值契约可施加更窄的限制。

图像 operation 按各自声明支持的语义描述符、布局、类型和 Region 执行。结构化 planar 路径要求 operation 声明 planar 能力并提供相应 callback；仅有旧式 image `Value` callback 的节点不因此获得 planar 执行能力。`SchemaTemplate::validate` 会拒绝旧 layer 图像 schema 并返回 `TypeMismatch`，要求使用 planar storage owner；拒绝分支见[`result.cpp`](../../../src/lib/data/result.cpp)。Layer operation 的规格入口并不表示旧 `Result` schema 路径仍可用。

## 当前能力入口

| 能力 | 实现子集与调用入口 | 数据和执行边界 |
| --- | --- | --- |
| Numeric、expression 与 LUT | 默认 registry 的数值、curve、field、统计和 expression/LUT operation；从[基础算子](../../kernel-architecture/Basic-Operations.md)、[数值操作](../../kernel-architecture/Numeric-Operations.md)、[表达式与 LUT](../../kernel-architecture/Expression-and-LUT-Operations.md)查看 key 契约；[Foundations workflow](../../../examples/foundations_workflow/README.md)提供可运行的 numeric 与 expression/LUT 场景。 | 仅按具体 key 公开的 dtype、shape、参数和 Whole/Region 规则执行；输入为 generic `Value` 不等于图像 planar 能力。 |
| Channel 与 color | 默认 registry 中明确注册的通道构造、提取、颜色模型转换和 RGB 基底节点，见[通道与颜色操作](../../kernel-architecture/Channel-and-Color-Operations.md)。 | 各节点只接受其声明的 facet、dtype 与布局；不提供任意图像格式转换或 ICC/OCIO 管理。 |
| Image 与 mask | legacy `Value` 回调包括 exposure、opacity、mask、source-over、box downsample、brush、mix、STMap、horizontal split，以及受构建条件控制的 inpaint；详情见[图像操作](../../kernel-architecture/Image-Operations.md)。 | legacy image callback 不支持结构化 `PlanarImage`。带 `photospider.image` 的输入声明缺少 planar layout 时在输入校验中拒绝；结构化 planar 输入还要求 `planar_storage_capable` 和 planar callback。 |
| Dependency sampling | `numeric.radius_gather` 和 `numeric.radius_scatter` 可通过 generic `Value` 路径执行；`image.stmap` 的注册 helper 保留受限 bilinear sampling contract，见[依赖采样](../../kernel-architecture/Dependency-Sampling.md)。 | Radius operation 只接受其 generic Value 契约；STMap 的 legacy image dependency 调用会因结构化图像输入或推导出的图像输出缺少 planar storage 而被拒绝，helper 注册不构成可执行 workflow。 |
| Named multi-output | `image.split_horizontal` 提供 `full`、`left`、`right` 三个 Value 输出，见[多输出操作](../../kernel-architecture/Multi-Output-Operations.md)。 | 它使用 legacy image Value 路径，不获得 planar storage；joint 执行是否可用及独立端口需求以 operation 契约为准。 |
| Statistics、FFT 与 connected components | 调用方分别创建并注册 `make_statistics_operation`、`make_fft_operation`、`make_component_operation`；对应的[统计](../../kernel-architecture/Integer-Statistics.md)、[外轴 FFT](../../kernel-architecture/External-FFT.md)、[分页连通域](../../kernel-architecture/Paged-Components.md)指南和[工作流验收入口](../14-roadmap/workflows.md)记录当前子集。 | statistics 接受契约限定的 Int64 source/UInt8 mask 并发布分页结果；FFT 当前链覆盖 Float64 real 与 Full/R2CHalf spectrum；components 当前链覆盖 UInt8 mask、labels、area 和 filter。factory 不会自动注册到默认 registry。 |
| Layer 与 structured representations | `LayerPixel` 及其纯计算 helper 保留在[`data/layer.hpp`](../../../include/photospider/data/layer.hpp)；[`make_layer_operation`](../../../src/lib/plugin/layer_operation.cpp) 仍构造 operation definition，representation schema/spec API 构造数据描述。 | 当前旧 Layer Result schema 在 `SchemaTemplate::validate` 中返回 `TypeMismatch`；`validate_traits` 经 selected-output 与 operation-contract 校验会拒绝以这些旧 Layer Result schema 作为输出的 operation definition，因此这些 Layer Result operation 当前无法成功注册或执行。schema/spec 构造不代表 operation 注册；`examples/layer_workflow` 的旧 source 不是当前运行验收。边界见[Layer runtime](../../kernel-architecture/Layer-Runtime.md)。 |
| Local inpaint | 默认 registry 保留 `image.local_inpaint_navier_stokes_native_apple_silicon` 和可选 OpenCV key；实现与目标契约见[PNT-05A 实现说明](../09-composite/inpaint-ns-implementation.md)。 | 这些是 legacy image Value operation；当前结构化图像调用因缺少 planar storage 而返回 `TypeMismatch`，所以该 key 与其 Whole/input contract 不构成可执行 workflow。OpenCV adapter 还要求对应构建选项。 |

## Result、资源与错误边界

`Result` 的 `RuntimeCount` extent 可解析为空结果，空集合使用零行且不分配数据 backing。`CompleteBundle` 在 seal 前不发布；`StablePrefix` 与 `IndependentChunks` 当前都只提供有序字段前缀，已发布范围不可撤回，后续失败可保留已发布前缀；任意乱序范围发布不属于当前 API。见[Global Results](../../kernel-architecture/Global-Results.md)。

受管资源预算限制内核声明并计量的 managed capacity、work 和 stage，不构成进程 RSS 上限；调用方输入和未计量的系统/驱动分配不因此受限。见[Managed Resources](../../kernel-architecture/Managed-Resources.md)。普通 `execute` 对执行错误保持 fail-fast；`execute_atoms` 只为受支持的 managed CPU Value dependency plans 收集有界、带 scope 的 Atom observation，并拒绝 planar image plans。见[Atom errors and quality](../../kernel-architecture/Atom-Errors-and-Quality.md)。

## 规格状态、边界与修订入口

规格的 `Proposed` 或 `Accepted` 状态描述目标契约状态，不表示实现已经注册。oracle 覆盖、operation 注册、planar storage 能力和 CPU/GPU backend 支持是不同事实，应分别按对应实现文档核对。

通用路径光栅、任意 solver、RAW、时域、Deep 和 ML 等能力不由现有基础设施自动提供；只有当前 registry、factory 或明确执行接口实现的部分才属于可调用能力。按[规格模板](spec-template.md)选择规格、实现说明或 workflow 的维护位置。
