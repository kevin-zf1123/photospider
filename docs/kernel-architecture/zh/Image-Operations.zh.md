# 图像算子

## 范围与归属

图像使用 `layout.spatial = true` 的普通 Result tensor slot。内核负责 tensor 结构、授权覆盖、backing 与执行，不根据通道数推断 RGB、alpha、颜色模型或数值范围。每个消费算子显式接收所需的描述、几何、transfer 和采样参数。

内置图像与格式算子编译进 `Photospider::ops`。该目标提供 authoring 头文件，并依赖 `Photospider::ops_sdk` 与 kernel。共享描述、codec、ICC/OCIO 资源封装和纯描述编辑属于 `Photospider::ops_sdk`；仅使用 SDK 的消费者不需要算子注册表。精确参数与公式见[通道与颜色算子](Channel-and-Color-Operations.zh.md)及其链接的族规格。[英文页面](../Image-Operations.md)为权威说明。

## 生产算子族

| 算子族 | 生产实现与公共接口 | 当前行为 |
| --- | --- | --- |
| 通道提取 | `channel_extraction.cpp`；`photospider/ops/format/channel.hpp` | 六个按 profile 区分的 index/name Result 算子。显式描述可解析轴与选择器；样本精确复制。`split_channels` 展开为 index 节点。 |
| 通道组合与编辑 | `channel_assembly.cpp`；`channel_assembly.hpp`、`channel_editing.hpp` | 组合、拼接和映射组合复制七种 dtype。Swizzle/replace 是基于这些 key 和 scalar literal 的事务性 authoring helper。 |
| Literal-like fill | `channel_literal_like.cpp` | 仅请求 Descriptor support，重复已准备的同 dtype 位模式；支持 alpha 提取的不透明回退。 |
| Scalar literal | `channel_assembly.cpp` | 无输入 complete-input primitive，生成 shape `[1]`，用于显式 scalar/literal 通道来源。 |
| Alpha | `alpha_operations.cpp`、`alpha_common.hpp`、`alpha_authoring.cpp`；`alpha.hpp` | 九个原生 key 覆盖三种 CPU profile 的 associate、unassociate 和 set。Extract/remove 为 authoring 组合。 |
| 数值转换 | `numeric_conversion.cpp` | `numeric.convert_format_strict` 通过显式数值范围实现七种 dtype 间的 49 对转换。 |
| Transfer | `transfer.cpp`；SDK `photospider/ops/format/transfer.hpp` | Encode/decode key 使用显式 transfer 定义及选定 RGB/Gray group 或 raw 分量。SDK 头提供定义 codec。 |
| RGB basis 与白点 | `rgb_basis.cpp`；`rgb_basis.hpp`、SDK `rgb_geometry.hpp` | A/B/C key 执行 RGB/XYZ 与显式白点适应；D helper 组合这些节点。 |
| 颜色模型 | `model_conversion.cpp`、`model_math.cpp`、`model_simd.cpp`；`model_conversion.hpp` | A–R/T 提供 57 个按 profile 区分的 CPU key；S 降低为 MASK threshold key。 |
| 描述编辑 | SDK `common/domain/description_editor.cpp`；`photospider/ops/format/metadata.hpp` | `format::edit_description` 返回配置值，不创建图节点；`metadata.assign/remove` 未注册。 |
| LUT bake 与应用 | `plugins/ops/01-numeric/`；`photospider/ops/numeric/lut3d_bake.hpp` | 使用普通数值 Result 和显式配置；报告/表 schema 与校验属于 SDK，不属于内核空间存储机制。 |

当前 Result operation C ABI 为 4。CPU profile 为 `strict`、`accelerated_apple_silicon` 和 `accelerated_x86_64`；加速 profile 要求匹配的主机能力。注册不代表支持全部空间布局或 GPU 后端。

## Alpha 与模型算子

原生 alpha 算子接收一个 tensor ，保留 batch prefix，轴参数相对于 cell axes。完整 sample rank 至多为 8，sample count 至多为 2^40。关联算术使用同 dtype 的 Float32/Float64；提取、移除和分量复制保留七种受支持 dtype 的原始位模式。

FMT-04 associate/unassociate 总是物化，不提供 layout 参数。FMT-05A Set 接受 layout 策略。通用 Set view 必须证明单个 `CpuStorage` owner 与仿射映射；空间 Set view 仅支持 internal identity。Auto 只在 `ViewUnavailable` 时物化。发布受 Root 预算和取消控制并保持事务性；Empty observation 不保留运行 payload 状态。这些算子没有新增 GPU 后端。

`extract_alpha` 将已有通道降低为提取，不透明回退降低为 literal-like fill。`remove_alpha` 对带通道输入降低为 mapped assembly 并返回 `values`；其无轴 identity 分支使用 `core.identity` 并返回 `value`。两种组合都不向输出附着描述。调用方为后续消费者提供结果解释。

FMT-11 输入包含一个 Float32/Float64 tensor。完整 sample rank 至多为 8；sample count 服从 schema 可表示性与资源限制。选中样本通过普通 payload tile Need 获取，并按显式算子配置校验。Raw T 验证二级选择器；raw S 和 bypass 读取选中样本；常量 R 使用显式 descriptor-only 访问。Empty 请求无状态。Q 可保留可表示的通用或空间 view；其余成员物化并拒绝 forced view，包括仅 bypass 的请求。输出保留结构 schema identity、tensor key 和 batch axes，并将 `atomic_trailing_axes` 设为零。预期输出解释由算子及显式参数定义，不附着到 Result。

## 空间 Result 契约

通用空间 Tensor 具有显式的 cell-relative H/W 轴和可选 channel 元数据，包含 batch prefix 的完整 sample rank 至多为八。H/W 必须互异且不与 atomic 末尾轴重叠；允许其他 cell 轴，并在空间 tile 中完整覆盖。具体图像算子可按自身契约要求二维或三维 cell。逻辑轴顺序独立于物理布局。Kernel schema 没有 component-group 字段，atomic observation closure 与 tile 几何不同。

每个 Result 成员都是拥有独立网格的静态 Tensor。控制值先通过普通 payload Need 读取，再选择后续输入 tile。内核记录实际 tile 依赖和冻结共享证据，不使用 relation 角色或保证等级。空间 tile 完整覆盖所有非 H/W cell 轴，atomic closure 不缩小该覆盖。

Result owner 保留 schema、已认证 descriptor facts、backing、ObjectId association 和 view 所需 source owners。算子资源配置由 preparation、plan 和执行状态持有。Root 准入覆盖 work、stage、I/O、tile 证据与几何元数据、payload 和 owners。发布失败是 sticky；取消或 stale execution 在释放借用 phase 状态之前等待已准入 callback 退出。

另见[结构化 Result](Global-Results.zh.md)、[Dependency Data](Dependency-Data.zh.md)和[Tensor Storage and Region Access](../../kernel-specs/Tensor-Storage-and-Region-Access.md)。
