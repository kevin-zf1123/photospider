# 图像算子

## 范围与归属

图像使用 `layout.spatial = true` 的普通 Result tensor slot。内核负责 tensor 结构、授权覆盖、backing 与执行，不根据通道数推断 RGB、alpha、颜色模型或数值范围。每个消费算子显式接收所需的描述、几何、transfer 和采样参数。

内置图像与格式算子编译进 `Photospider::ops`。该目标提供 authoring 头文件，并依赖 `Photospider::ops_sdk` 与 kernel。共享描述、codec、ICC/OCIO 资源封装和纯描述编辑属于 `Photospider::ops_sdk`；仅使用 SDK 的消费者不需要算子注册表。精确参数与公式见[通道与颜色算子](Channel-and-Color-Operations.zh.md)及其链接的族规格。[英文页面](../Image-Operations.md)为权威说明。

## 生产算子族

| 算子族 | 生产实现与公共接口 | 当前行为 |
| --- | --- | --- |
| 通道提取 | `channel_extraction.cpp`；`photospider/ops/format/channel.hpp` | 六个固定 ABI 8 index/name CPU 算子，将选中样本精确复制到独立 dense tile，零 scratch；非空 view 失败。`split_channels` 展开为 index 节点。 |
| 通道组合与编辑 | `channel_assembly.cpp`；`channel_assembly.hpp`、`channel_editing.hpp` | 固定 ABI 8 组合、拼接和映射任务使用零 scratch，将七种 dtype 复制到独立 dense tile；Swizzle/replace 组合这些 key 和 scalar literal。 |
| Literal-like fill | `channel_literal_like.cpp` | 固定 ABI 8 CPU task 仅保留输入 schema 依赖，在选中输出 tile 中重复已准备的同 dtype 位模式，零 scratch；支持 alpha 提取的不透明回退。 |
| Scalar literal | `channel_assembly.cpp` | 零输入、零 scratch 的固定 ABI 8 task，逐位生成 shape `[1]` tile，用于显式 scalar/literal 通道来源。 |
| Alpha | `alpha_operations.cpp`、`alpha_common.hpp`、`alpha_authoring.cpp`；`alpha.hpp` | 九个固定 ABI 8 key 覆盖三种 CPU profile 的 associate、unassociate 和 set。Extract/remove 为 authoring 组合。 |
| 数值转换 | `numeric_conversion.cpp` | `numeric.convert_format_strict` 通过固定 ABI 8 和显式数值范围实现七种 dtype 间的 49 对转换，使用固定精确 scratch 与独立 dense 输出 tile。 |
| Transfer | `transfer.cpp`；SDK `photospider/ops/format/transfer.hpp` | 六个固定 ABI 8 CPU encode/decode keys 使用显式 transfer 定义及选定 RGB/Gray group 或 raw 分量，声明源 tile 闭包、数值 scratch 和独立输出。语义样本有效性属于调用者前提；SDK 提供定义 codec。 |
| RGB basis 与白点 | `rgb_basis.cpp`；`rgb_basis.hpp`、SDK `rgb_geometry.hpp` | 九个 A/B/C 固定 ABI 8 CPU keys 执行 RGB/XYZ 与显式白点适应，使用静态源 tile 闭包、固定 scratch 和独立 dense 输出。语义样本有效性属于调用者前提；D 保留各阶段舍入。 |
| 颜色模型 | `model_conversion.cpp`、`model_math.cpp`、`model_simd.cpp`；`model_conversion.hpp` | A–R/T 提供 57 个固定 ABI 8 CPU keys；S 降低为三个固定 MASK threshold keys。 |
| 色域与亮度映射 | `gamut_tone.cpp`、`color_point.hpp` | FMT-14 A-D 和 FMT-15 A/B 的六个 strict 固定 ABI 8 CPU keys；Float32/64、精确组件依赖、有限域错误、独立输出和固定算术 scratch。 |
| ICC、OCIO 与打样 | icc_operations.cpp、ocio_operations.cpp；format/external_color.hpp | FMT-12 A-C、FMT-13 A-F 和 FMT-18 A/B 共十一个固定 ABI 8 CPU keys；私有固定引擎、Float32/64、闭合不可变资源、精确颜色/旁路依赖。 |
| 显式 view | `view_authoring.cpp`；`photospider/ops/format/view.hpp` | `format::compose_view` 事务式组合 native FMT-09/10/14/15 与 configured FMT-13A..F 阶段；资源感知重载验证真实 processor 及 native/configured 最终显示断言。 |
| 描述编辑 | SDK `common/domain/description_editor.cpp`；`photospider/ops/format/metadata.hpp` | `format::edit_description` 返回配置值，不创建图节点；`metadata.assign/remove` 未注册。 |
| LUT bake 与应用 | `plugins/ops/01-numeric/`；`photospider/ops/numeric/lut3d_bake.hpp` | 使用普通数值 Result 和显式配置；报告/表 schema 与校验属于 SDK，不属于内核空间存储机制。 |

各族的运行调用协议见[通道与颜色算子](Channel-and-Color-Operations.zh.md)。原生数值 CPU profile 为 `strict`、`accelerated_apple_silicon` 和 `accelerated_x86_64`；加速 profile 要求匹配的主机能力。注册不代表支持全部空间布局或 GPU 后端。

外部引擎构建同步且不可取消，保留内存/工作量准入与 Empty/bypass 完整验证；像素执行继续支持取消，无需改变冻结内核。详见[实现、独立验证与性能](../../built-in_ops/02-format-color/FMT-12-13-18_implementation.md)。

## Alpha 与模型算子

原生 alpha 算子接收一个 tensor ，保留 batch prefix，轴参数相对于 cell axes。完整 sample rank 至多为 8，sample count 至多为 2^40。关联算术使用同 dtype 的 Float32/Float64；提取、移除和分量复制保留七种受支持 dtype 的原始位模式。

FMT-04 associate/unassociate 使用固定输入 tile 并集与独立 dense 输出，不提供 layout 参数。语义数值有效性是调用者前提，raw IEEE 结果仍有定义。纯复制 task 声明零 scratch，算术 task 为 4096 字节，reference 算术为 6888 字节。FMT-05A Set 的 auto/materialize 以零 scratch 复制独立输出；非空 view 仅依赖 schema 并返回 ViewUnavailable，Empty 不生成 task。公共发布服从成功调用的顺序退休。这些算子不新增 GPU backend。

`extract_alpha` 将已有通道降低为提取，不透明回退降低为 literal-like fill。`remove_alpha` 对带通道输入降低为 mapped assembly 并返回 `values`；其无轴 identity 分支使用 `core.identity` 并返回 `value`。两种组合都不向输出附着描述。调用方为后续消费者提供结果解释。

FMT-11 输入包含一个 Float32/Float64 tensor。完整 sample rank 至多为 8；sample count 服从 schema 可表示性与资源限制。每个选中输出 tile 对应固定 task，其源读取按分量映射在输入网格上取闭包。语义数值有效性与 T 的二值 selector 属于调用者前提。不含其他源依赖输出的 R 常量 task 仅依赖 schema，使用零 scratch；纯复制也使用零 scratch。Auto/materialize 产生独立 dense 输出。Q 的非空 forced view task 仅依赖 schema，并返回 ViewUnavailable；其余成员静态拒绝 forced view。Empty 不生成 payload task。输出保留结构 schema identity、tensor key 和 batch axes，并将 atomic_trailing_axes 设为零。输出解释由显式配置提供，不附着到 Result。

## 空间 Result 契约

通用空间 Tensor 具有显式的 cell-relative H/W 轴和可选 channel 元数据，包含 batch prefix 的完整 sample rank 至多为八。H/W 必须互异且不与 atomic 末尾轴重叠；允许其他 cell 轴，并在空间 tile 中完整覆盖。具体图像算子可按自身契约要求二维或三维 cell。逻辑轴顺序独立于物理布局。Kernel schema 没有 component-group 字段，atomic observation closure 与 tile 几何不同。

每个 Result 成员都有独立的静态 tile 网格。固定算子的输入闭包、COPY 和任务依赖在编译时确定；运行时按完成事件调度，并按调用顺序退休。每个 produced tile 使用独立的预分配存储，源绑定的具体 stride、offset 和 capacity 在执行前校验。空间元数据描述轴拓扑，与这些物理视图分开。

已接入固定执行器的提取、组装、literal 和相关 helper 使用本文列出的 ABI 8 路径。其余族的调用与存储机制按各自规格说明。Result owner 保留实际存储；固定执行器在取消或失败后排空已提交任务，再释放借用输入和任务状态。

另见[结构化 Result](Global-Results.zh.md)、[Dependency Data](Dependency-Data.zh.md)和[Tensor Storage and Region Access](../../kernel-specs/Tensor-Storage-and-Region-Access.md)。
