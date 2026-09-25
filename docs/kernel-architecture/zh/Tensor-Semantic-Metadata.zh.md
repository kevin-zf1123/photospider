# 张量语义元数据与 FMT-08

英文权威版本：[Tensor semantic metadata and FMT-08](../Tensor-Semantic-Metadata.md)。

包 0.22.0 实现 FMT-08A 的 `metadata.assign_strict`、
`metadata.assign_accelerated_apple_silicon`、`metadata.assign_accelerated_x86_64`。
FMT-08B 是事务式公开 helper `format::remove_metadata`，展开为没有 set 条目的 A
节点。各 CPU profile 保持原有后端准入规则，样本位完全一致；没有原生删除 key
或仅按样本缓存的结果。

## 版本与解释

`photospider.tensor-description` 只接受运行时版本 **4** 和 `TDM4` 标识。
拒绝 v1-v3；消费者必须重新编译并显式重新编写元数据，不会给旧字节自动赋新单位。
WorkflowDocument 和 operation/provider C ABI 版本不变。canonical facet 与静态
编辑参数参与编译身份。旧 ColorArray v1 保持独立的旧坐标消费约定，不能与 v4
共存于一个 Value。FMT-08 拒绝旧 typed facet。其他 FMT 数值与外部引擎算子的
实现状态仍以各自规范为准。

`relative-v1` 明确 CIELAB/CIELCh 的 `l=L*/100`、不变的 a/b/chroma，以及 XYZ
参考白 Y=1 的原有比例；该约定不实施数值裁剪。`icc-native`、`ocio-native`
表示资源定义的坐标。绝对单位仍由分量/轴字符串显式声明，赋值不会缩放样本。

`TensorDescription` 的 canonical 编码上限为 4096 字节。文本为最长 128 字节
的严格 UTF-8；最多 128 个完整组，每组最多 64 个分量。整数与 IEEE binary64
按小端字节编码，presence 为 0/1，通道/轴表有序，资源身份显式。v4 另用有界的
小端 base-2^32 分子和正分母 limbs 编码约分后的有符号精确有理数端点；该变体
改变安装包 C++ ABI，并在整数或 binary64 不能表示组合 decoder 端点时保留精确值。
解码后重编码
核对 canonical 字节并拒绝尾随数据。opaque annotation 使用独立 `ValueFacet`，
不是语义字段，遵循宿主最多 64 个 facet、每个 64 KiB、总计 1 MiB 的限制。

## 已注册 schema

| 记录 | 字段与含义 |
| --- | --- |
| Tensor | channel_axis、channels、component、axes、groups，以及全局 encoding/sampling/interpretation 默认值 |
| Component | name、role、unit，以及可选 interpretation、encoding、sampling |
| Axis | name、unit、有限 origin、有限正 step；解释世界坐标，不改变索引 |
| Group | 唯一 name、有序且不重复的 indices、对应 components、完整 interpretation、可选同 tensor 内部 alpha；alpha 不得与颜色索引重合 |
| Interpretation | model、primaries、transfer、reference、association、white、primaries_xy、profile、convention、configured、analytic_binding |
| Encoding | 精确类型的 stored/decoded 端点对；stored 递增，decoded 不相等且可反向 |
| Sampling | 显式 grid；内部同尺寸、同位平面要求 scale=(1,1)、offset=(0,0)，外部 subsampling 被拒绝 |
| Configured space | 冻结 config 身份、显式 canonical space、scene/display reference_space |
| Analytic binding | 调用者声明的模型、基色、传递、参考、白点、有序角色/单位及 relative-v1 约定 |

`TensorEndpoint` 是 Int64 或有限 Float64，序列化保留类型标签和精确位；Int64
最大值不会经 Float64 中转。编码解释为：
`D(x)=decoded[0]+(x-stored[0])*(decoded[1]-decoded[0])/(stored[1]-stored[0])`。
存储区间必须适配 dtype。完整整数颜色组必须具有显式 decoder，可以来自组分量、
通道或 tensor 默认值。删除必要 decoder 后不能保留完整 native-color 声明。
显式分量/组之间的编码或 sampling grid 冲突会失败。

完整组检查模型角色、索引、alpha、必需的色彩空间信息、采样及 analytic binding
的顺序/单位。独立分量可以保留不完整的描述来源信息。profile/configured space
不会生成猜测的基色、传递函数或白点；analytic binding 是调用者断言，不是等价
证明。任何字段都不会认证样本有限性、coverage 范围或预乘零值条件。

## 冻结资源

`ResourceBindings` 封存并去重 ICC profile 与 `OcioConfigResource`。编译、绑定及
输出准入时所有引用身份必须可解析；结果头只保留 facet 实际引用的资源。view
仍可独立保留旧 backing，因此删除结果头引用不保证所有祖先分配立即释放。

ICC 准入检查显式 v2/v4 字节、header 模型/class/PCS、必需 tag、TRC/LUT 边界和
profile ID。支持 RGB/Gray matrix/TRC 或已准入 LUT profile、既有 CMYK output
路径及 profile-defined XYZ/Lab LUT 空间。Abstract/DeviceLink 不作为端点资源。
header model 必须与声明一致，也检查旧 CMYK 约定。此结构检查不运行 CMM，不能
证明 FMT-12 引擎兼容性。

OCIO 快照包含显式 config 字节、完整且排序的逻辑文件映射、已解析 context、
调用者声明的 canonical space/reference，以及固定 engine/build/settings 身份。
SHA-256 与字节长度覆盖全部分帧字节。缺失的查询保持缺失，不访问文件、网络或
进程环境。准入验证快照 schema 和所有权，不解析 OCIO YAML 或证明变换可执行。
未来 FMT-13 processor 必须用固定引擎核验空间与所选传递依赖，且只能访问该封闭
快照。此实现完成带资源引用的描述 schema，不宣称已实现 OCIO 变换。

快照复制、owner 元数据及跨 root 引用计入 ResourceBudget；复制/哈希每至多
1024 字节检查取消和 work。查询最多扫描 1024 个声明条目。准入失败释放未发布
owner。`ValueFragments::retained_bytes` 按实际存储身份去重并包含配置快照。

## 原子 authoring 与路径编码

`format::assign_metadata(document,input,MetadataOptions)` 追加一个节点，默认
patch/error/error/auto/strict，分别对应 mode/dependencies/missing/layout/profile。
replace 必须提供完整 description（可为空），保留 annotation，并只允许显式编辑
annotation；patch 禁止 description。未知选项、路径、类型、重叠编辑及非法静态
记录不会追加 helper 节点。依赖源描述的选择器在编译时验证。

路径以 `/` 分段，用 `~0` 转义 `~`、`~1` 转义 `/`：

- `/semantic` 是完整语义记录。
- `/semantic/channels/index:0/unit` 也可以使用 `name:R` 或 `role:red`；必须在
  原输入中唯一精确匹配。missing=ignore 可以忽略零匹配删除，歧义始终失败。
- `/semantic/groups/color/interpretation/primaries` 是组内字段。
- `/semantic/axes/0/origin` 是轴内字段。
- `/annotations/app.note` 是 opaque facet；`photospider.` 前缀为保留语义命名空间。

所有选择器在编辑前对原输入解析。设置整个子树会替换它全部内容，设置叶子保留
适用兄弟字段。重复、祖先/后代冲突及选择器别名冲突失败。删除通道/轴描述只重置
对应描述记录，数据槽位不变。

节点 `edits` String 是 v1 事务记录的小写十六进制。记录包含有序 version、set、
remove 和可选 description。树编码为 `kind+十进制长度或条目数+':'+内容`；o 为
有序 map、s 为字符串、u/i 为无符号/精确有符号整数、d 为 8 字节小端 Float64、
b 为不透明字节。解码限制为 4096 字节、1024 节点、深度 12，String 上限 8192。
事务编码与发布的 TDM4 编码独立，推荐使用公开的类型化 helper。

Cascade 依照封闭 schema 依赖处理：channel axis 关联通道表和组；encoding/
sampling/configured/profile 关联包含它们的解释单元；完整组关联必需字段、引用
的分量声明及重叠的显式赋值。只处理受影响单元，保留独立名称、单位及数据平面，
拒绝删除新显式赋值。最终一次验证并发布不可变候选，不改源头或修复样本。

静态准备受 facet/事务上限约束，使用编译/准备存储；既有 OperationPreparer
接口不提供运行时取消 token。运行时发布按实际 facet 容量、窗口、owner 与请求
backing 准入。不引入图像规模的元数据 scratch、隐藏 alpha 绑定或私有线程池。

## 精确执行与验收

输出每个坐标只依赖相同输入坐标；静态描述/资源检查不增加像素 Validation 或
Control 支持。数据 dirty 映射为 identity，必需上游失败保持原有作用域。

Auto/view 在既有不可变 backing 上发布独立元数据和精确有效覆盖；materialize
总是新分配请求区域。generic 路径用于非图像数值张量的合法正/负/零 stride；
continuous/tiled 图像路径保持 planar、单物理 owner 和 DAG tile geometry。
直接 planar invocation 已由调用者提供 writer，forced view 报 ViewUnavailable；
公开 compile/execute 可以发布 retained image view。连续复制不跨越请求、fragment
或物理 tile 边界，每至多 1024 样本检查取消/currentness。

[最小公开示例](../../../examples/metadata_workflow/README.md) 与
`test_metadata_assignment` 检查位保真、不可变元数据、七种 dtype、rank 1..8、
layout、稀疏跨 tile 请求、缺失覆盖、路径/cascade/stride、v3 端点/资源及错误回滚。
`photospider_metadata_consumer` 使用隔离安装包运行同一验收。
[性能说明](../../../examples/metadata_performance/README.md) 分开报告静态准备、公开
执行、算子内部耗时及保留/新增 backing，未启用仅样本缓存。
