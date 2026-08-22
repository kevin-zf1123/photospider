# ADR 0013：普通 DenseImage 坐标、样本、颜色与统计是分离契约

## 状态

本决策于 2026-08-17 为 GitHub Issue #129 / DI-1 接受，约束内建普通 DenseImage metadata
基线。DI-3 现在通过纯 C operation ABI v1 投影完整 metadata；DI-4 已完成 Host、IPC、worker、
durable、codec 与 CLI 迁移，并删除此前的兼容图像类型。本 ADR 不实现自动颜色转换，也不把 provider-defined OpenEXR
Deep metadata 重新解释为普通图像权威。

英文架构与 OpenSpec 文档是权威来源。实时远端交付、CI 与 review 状态仍由
Project 6、issue #129、active OpenSpec change 与 `development_tracking.md` 维护。

## 背景

ADR 0008 已把 `DenseTensor + ImageFacet` 确立为普通图像模型，但已安装的
ImageFacet 过去只携带 x/y/可选 channel axes。这使逻辑原点、显示范围、稳定
通道身份、声明样本含义、颜色解释与观测统计仍可能依赖隐式零原点、通道名称
或 `[0,1]` 假设。

这些事实并不共享同一生命周期或身份：

- 载荷坐标是不可变描述符事实；
- 显示范围是不可变呈现元数据；
- `RegionSet` 是动态工作或有效性；
- 存储能力、量化、声明样本区间与颜色解释可以独立变化；
- 观测 min/max 与 histogram 依赖载荷 revision、Region、选择器、算法和版本；
- 诊断名称可以在不改变语义的情况下变化。

后续 output-plan、operation-plugin、wire、codec 与 artifact 迁移需要同一个
有界记录基线。向结构版本 1 追加可选字段会让新旧载荷含糊，并诱发被禁止的
missing-tail 默认值。

## 决策

### 坐标与就绪状态

`ImageBounds` 用四个有符号 64 位端点表示非空半开窗口。每个内建普通
ImageFacet 都必须具有 `data_window`；可选 `display_window` 与其相互独立。
发布会校验不发生有符号溢出的有序跨度、跨度与描述符显式 x/y axes 的精确
一致性，以及 x/y/可选 channel axes 在 rank 内且互异。

数据窗口是逻辑像素坐标权威。显示窗口不授予载荷访问。动态 dirty、dependency、
execution 与 HP-validity 选择仍属于 `RegionSet`。完整 ImageRect 就是精确数据
窗口，operation 代码只在包含关系校验后减去其有符号原点。

`Value::image_bounds()` 无需轮询就绪状态或获取载荷租约即可读取保留元数据。
Pending、Failed 与 ProducerCancelled Value 保留该访问；`buffer_handle()`、
`DenseTensorView` 与 `ImageView` 载荷构造仍只允许 Ready。`ImageView` 保留零基
存储索引访问，并新增独立的有符号逻辑坐标访问器。

### 通道、样本、存储与颜色

可选有界 `ChannelSchema` 为 channel-axis 每个元素包含一个非零唯一
`ChannelId`，并包含按规范排序、成员有效且无重复的 `ChannelGroupId` 记录。
通道与分组名称只是有界诊断信息，不影响角色、语义相等性、descriptor digest
或 content digest。

存储可表示范围只从 `ElementSemantics + StorageEncoding` 推导。
`QuantizationSchema` 保持正交。版本化 `SampleEncoding` 与 `SampleDomainFacet`
声明 normalized、legal 或 code-value 的有限闭区间，并允许以稳定 ID 为键的
有界逐通道覆盖。声明域既不改变存储能力，也不授权转换。

版本化 `ColorFacet` 把一个现存非空 `ChannelGroupId` 绑定到显式
scene-linear/sRGB/Rec.709/PQ/HLG 传递函数，以及显式
Rec.709/Display-P3-D65/Rec.2020/ACES-AP0/ACES-AP1 色原色。scene linearity
是颜色事实，不是 sample-domain 标志。名称永不隐含 RGB 或 alpha。

`ImageFacet` 是这些内建普通图像事实唯一的 C++ 所有者。规范编码仍分别发出
独立 Image、Sample Domain 与 Color Facet 记录，使这些含义可独立版本化且不
创建第二 Value 权威。

组合多个图像输入的 operation 只能投影所有参与输入均已证明兼容的 metadata
事实。当 output geometry 不变时，它可以保留 primary input 的有符号 data window
与 display window。只有 output channel cardinality 不变、没有发生显式 channel
remapping，且每个输入都声明语义相等的 channel 事实时，channel schema 才能保留。
color interpretation 还要求该 schema 已保留，且每个输入都声明语义相等的 color 事实。
只有每个输入都声明完全相同的 sample domain，
并且都没有 per-channel override 时，uniform sample domain 才能保留。否则必须省略
对应的 optional fact，使 downstream consumer fail closed。payload value 绝不暗示
sample domain、不授权 sample conversion，也不选择 channel role。

### 观测统计

观测 min/max 与固定 bin histogram 是独立有界派生数据记录，永远不是
ImageFacet 或 Value 成员。query 包含规范 `RegionSet`、恰好一个稳定
channel/group 选择器、算法、正算法版本和算法参数。cache key 包含有效
`ValueRevisionId`、可选 `ContentDigest` 与完整 query。result 包含有界、按稳定
ID 排序的逐通道计数、有限极值、显式 NaN/infinity 计数，以及请求的 histogram
计数。

创建、替换或驱逐统计结果不能改变 Value revision、descriptor/content 身份、
正式 cache 有效性或 artifact 身份。DI-1 定义并校验记录。DI-2 在
`GraphCacheService` 中安装唯一 process-local derived-statistics cache；其 scheduling
key 包含完整 `ValueRevisionId` 与完整 query，accepted task 会保留 input `Value`，
而 cancellation、execution failure、revision invalidation、eviction 或 explicit clearing
都不能修改 Value 或 formal-cache authority。

### 规范版本与兼容边界

内建 DenseTensor Schema 升级到结构版本 2，其既有载荷字段顺序保持固定。
Image Facet 升级到结构版本 2，编码 axes、有符号 data/display windows、稳定
通道顺序、group IDs 与成员关系。独立 Sample Domain 与 Color 记录从结构版本
1 开始。有符号整数使用二补码小端；有限 binary64 元数据把两种有符号零规范
为正零。诊断名称、就绪状态、binding、lease 与观测统计被排除。

不存在结构版本 1 fallback、兼容别名或 missing-tail 猜测。后续 decoder 必须
选择精确受支持版本，并拒绝任何其他结构。

在 DI-1 过渡期间，此前的 image-to-Value bridge 会创建显式零原点数据窗口，且不携带
display/channel-schema/sample/color 事实。反向投影会复制活跃元素，并因为前身类型无法表示
丰富元数据而有意识地丢失它们。DI-4 已删除该 bridge 的两个方向。Isolated CPU protocol v2
则编码完整的可选 ImageFacet 记录。图像 output 会保留 axes、有符号 window、channel、
sample domain 与 color 事实；generic DenseTensor output 完全不携带 facet。任何
presence/identity 不匹配都会 fail closed，而不会被静默合成或丢弃。

## 后果

- 负原点与非零原点普通图像是一等且确定的。
- 每个仓库自有 ImageFacet 构造都必须提供显式边界；不存在 seal-time 默认或
  兼容 overload。
- 纯描述符推导与保留解释的内建 operation 无需载荷访问即可复制完整有界
  ImageFacet。
- 规范 golden digest 随结构版本 2 有意改变。
- 诊断拼写与派生统计不会扰动语义身份。
- DI-2 针对完整 frozen facet 与 Value revision 调度派生统计；该切片当时存在的 compatibility
  projection 从未成为 statistics identity 或 cache-key authority，并且现已删除。
- 产品 wire/artifact 边界会精确编码冻结记录，否则拒绝它们。Operation ABI v1、isolated CPU
  protocol v2、IPC named artifact、worker protocol v3 与 durable manifest 都保留各自实现记录。
- OpenEXR Deep provider 窗口仍是 provider-defined 元数据，不会复用为内建普通
  DenseImage 权威。

## 参考资料

- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
- [Kernel 数据模型](../../kernel-architecture/zh/Data-Model.zh.md)
- [稠密图像 Value 内存契约](../../kernel-architecture/zh/Dense-Image-Value-Memory-Contract.zh.md)
- [Kernel 缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md)
- [普通图像 Value 迁移](../../roadmap/zh/Kernel-Evolution.zh.md#普通图像-value-迁移)
- GitHub Project 6 / parent issue #128 / issues #129、#130 与 #131
- OpenSpec change `define-dense-image-coordinate-sample-statistics-contracts`
- OpenSpec change `add-host-owned-output-authorization`
