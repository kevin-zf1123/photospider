---
spec_schema_version: 1
id: KERNEL-tensor-storage-zh
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
clarification_status: selected_storage_policy_complete
---

# 张量存储与区域访问

本文是 package 0.29.0、WorkflowDocument schema 4 的 CPU 存储契约译文，
[英文版本](../Tensor-Storage-and-Region-Access.md)为权威。本文负责物理布局、tile
几何和区域访问。结构化 `ResultRef` 是图像数据唯一的语义、发布、input/output 和
生命周期 owner；`PlanarImage` 仅作为 image slots 内部的 private typed backing，
不创建并列 public image result 或执行路径。颜色与 alpha 解释由
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义。本契约不保留已退休图像的内存／数值接口，也不提供兼容适配。

补充的[codec 边界澄清](../../built-in_ops/02-format-color/op_specs/FMT_codec_boundary.md)
要求同一图像的颜色／alpha 平面同尺寸、同采样网格，包括全分辨率 Y/Cb/Cr。
外部色度子采样及布局／位打包归 input/output codec，FMT-16/17 已退休。
Interleaved 导入须经 codec 转为 planar 后进入内核。这是图像边界澄清，
不宣称 codec 已实现，也不改变下方存储寻址公式。

## 已确认决定

| 主题 | 目标 |
| --- | --- |
| 图像布局 | 所有图像必须 planar；交错图像导入时显式转换布局。 |
| DAG tile 策略 | 同一 DAG 统一 tile 尺寸，各算子不能独立决定输出 tile 尺寸。高宽必须分别为正的 2 次幂（包括 1），不支持非 2 次幂 tile。64×64、128×128、256×256 是示例。 |
| 平面组织 | 保留连续平面模式；采用 tile 时，所有分块图像平面服从 DAG 配置，不提供逐平面尺寸覆盖。 |
| backing | 预留整图连续虚拟地址范围，按需提供页 backing；平面／tile 访问共享图像地址空间 owner，不另建互不相关的图像分配。 |
| 行 | 平面／tile 的行内样本连续，允许行尾 padding。 |
| 颜色与 alpha | 同一张量可包含颜色组和独立 alpha 平面，例如 L*、a*、b*、Alpha；alpha 不属于 Lab 颜色坐标。 |
| 边缘 tile | 只保留有效行，每行宽度补齐到统一 tile 宽度；中间块 payload 为紧密完整块。下一 tile 起点强制页对齐，地址间隙与行 padding 分开；不补缺少的底部行。 |
| 页准备 | 算子执行前显式取得、准备所需页，检查资源与取消；不在缺页异常中调度 DAG 计算。 |
| 页保留 | 已产生数据的页 backing 保留到图像最后一个生命周期 owner 释放；超预算失败，不自动驱逐、DAG 回算或临时文件换页。 |

DAG 统一配置是当前 tile 策略。每个 typed image slot 为每个 frame/layer pair
拥有一份 planar backing；每份 backing 使用整图连续虚拟地址范围，并按需提供页面。
Typed Result image slot 是当前图像语义载体；旧 Image/Layer Value 特殊类型仍已退役。
张量 shape/dtype、物理布局、颜色组与实际消费的 metadata 继续分离。raw／override
不能改变真实地址映射，也不能通过改标签把交错图像变成平面图像。

## 逻辑坐标与布局

逻辑轴顺序显式声明，平面式物理存储不强制所有张量采用 CHW。无 padding 且各平面
紧密相接时，同一份数据采用逻辑 HWC 的 byte stride 为 [W*d,d,H*W*d]，采用 CHW
则为 [H*W*d,W*d,d]；d 是元素字节数。shape 与轴变换独立于颜色解释检查。

标准物化图像的相邻行内样本使用正 byte stride d。连续平面允许经检查的行尾
padding；分块模式所有行 pitch 均为 Tw*d，包括右边缘块。行宽补齐与 tile 起点
页对齐分别生效。普通张量继续允许一般 strided view；例如交错
RGBA 中 pixel stride 为 4*d 的 R view 不满足标准图像存储要求，发布为图像前
需要显式物理转换。平面行的普通 ROI view 可以保留 owner 和行 pitch，无需复制整行。

同一张量保持统一 dtype 和 shape 关系。颜色组与 alpha 可以共享载体并保留独立
语义；本契约不定义预乘 Lab，也不对 alpha 执行颜色 transfer。Semantic Image 和
ColorArray facets 保留其既有完整 channel tuple 闭包；若 alpha 是一个 channel，
闭包也包含 alpha。只有 TDM 的 facets 和 `PlanarImageLayout::groups` 描述 metadata
或物理组织，不会增加 peer-channel 或 alpha sample demand。Operation 声明的 sample
needs 仍是 dependency 的权威依据。

Structured image slot 的每个 backing descriptor 保留自身声明的轴顺序。`{H,W}` 和
`{H,W,C}` 是常见示例，不是固定轴顺序。CHW backing 使用 descriptor shape `{C,H,W}`，
并将 channel、height、width 分别映射到 descriptor axes 0、1、2。Result logical coordinates
在样本请求中为该 backing 前置 frame 和 layer，因此表示为 `{N,L,C,H,W}`。`N`、`L` 为正数，
`N*L` 最多 4096。Slot schema 保留 frame/layer identity；plane 和 tile 公式分别用于每个
backing。Frame 和 layer 不压入 color channels。

## 连续平面与分块平面

连续模式中，每个平面按行 pitch 占据共同 backing 的一段，按声明通道顺序排列，
有显式偏移和对齐间隙。分块模式在每个平面内按 tile 行、tile 列排列，一个平面的
所有 tile 位于下一平面之前，即 R tiles、G tiles、B／alpha tiles。每个 tile 起点
页对齐，跨到下一平面时也如此。行 padding、块间地址间隙与最终地址预留取整
均不是逻辑像素。

设统一 tile 几何为 (Th,Tw)，平面尺寸 H,W。网格以图像坐标 (0,0) 为起点，不随
请求 ROI 改变。像素 (y,x) 对应 tile (floor(y/Th),floor(x/Tw))，块内坐标为
(y mod Th,x mod Tw)。有效尺寸和偏移可用以下公式检查与计算，不必为每个 tile
分配目录项。整张分块平面通常不能用一组固定全局 stride 表达。

对有效尺寸 ht、wt、起点 ot 的 tile：

```
row_pitch     = Tw*d
valid_bytes   = ht*wt*d
padding_bytes = ht*(Tw-wt)*d
storage_span  = ht*Tw*d
next_offset   = align_up(ot + storage_span, P)
alignment_gap = next_offset - (ot + storage_span)
```

中间块 ht=Th、wt=Tw，payload 为紧密 Th*Tw 块。右边缘只在各有效行末尾填充；
底部保留实际行数，不预留缺少的 Th-ht 行。因此 T=128 时，有效 2×72 的边缘块
保存为 2×128 个样本槽位，下一块仍页对齐。P 为主机页大小，图像基址页对齐。
中间块也遵守起点页对齐：payload 小于 P 时，后面可以有地址间隙。整图基址与
预留大小仍遵循平台粒度。

设 nx=ceil(W/Tw)、q=floor(H/Th)、hrem=H mod Th，同一 tile 行的有效高度相同：

```
full_step = align_up(Th*Tw*d, P)
edge_step = 0 if hrem=0 else align_up(hrem*Tw*d, P)
plane_step = nx*(q*full_step + edge_step)
ht = min(Th, H-ty*Th)
tile_offset = plane_offset + ty*nx*full_step + tx*align_up(ht*Tw*d, P)
sample_offset(y,x) = tile_offset + (y mod Th)*Tw*d + (x mod Tw)*d
```

同尺寸平面的 plane_offset=c*plane_step。图像范围包含已对齐的块槽位，最终平台
预留取整可能增加尾部。每块只有 ht*Tw*d 字节属于块内存储段；页 backing 根据
实际获准样本字节范围准备。页对齐不赋予整页有效样本覆盖。

块数为 ceil(H/Th)*ceil(W/Tw)，有效范围裁剪至 H,W。padding 不是有效样本、隐式
零像素或滤波边界规则，不影响颜色结果、样本身份或 dirty 映射。字节量、pitch、
偏移、对齐取整和 shape 乘积必须在按规模分配前检查溢出。

**tile 高度和宽度必须分别为正的 2 次幂，不支持非 2 次幂 tile。**
`Compiler::plan` 以 InvalidArgument 拒绝不符合的 DAG tile 几何；`ResultBuilder` 创建内部
backing 时也会校验 tile 几何，continuous 存储同样遵循此约束。图像和 ROI 的有效尺寸可为任意正值；
不足整 tile 的边缘保持实际有效范围。校验后的几何允许用移位和掩码计算地址。

tile 大小是 DAG 策略，不是算子参数。128×128 默认值保留且可配置，不
限制为唯一尺寸。halo 与跨 tile ROI 可以超过一块，但不改变输出块几何。无图像
空间轴的普通数值张量不凭空增加图像轴。不合规的图像存储通过显式导入／布局
转换满足 planar 和 DAG tile 配置。

## backing、view 与精确访问

连续指具有共享生命周期的整图预留虚拟字节范围，包含对齐间隙，不要求物理 RAM
页相邻，也不要求一次提供整图 backing。每个坐标在范围内有稳定偏移。内部 plane/tile
访问保留有界窗口所需 backing。Owning Result 保留全部 frame/layer backing 及含有已发布
数据的页面。关闭内部访问窗口不会在 Result 存活期间驱逐页面。最后一个 Result owner
释放图像页、虚拟地址范围和保留的输入 associations。

Image access request 使用 Result schema 和精确逻辑 sample set。Coordinator 将需求
映射到 page/tile portions；typed `ResultImageInput` 只能在 captured descriptor、
selected slot、已发布 coverage 和声明 Need 范围内读取。C++ 或 C callback 通过
Result service 读取，不能构造或保留独立 `PlanarImage` owner 或获取独立窗口。
跨 tile 不要求收集整图。物理 page/transport 粒度可能超过逻辑请求，需分别计费。
`ResultBuilder::publish_image` 写入精确 output regions，并在发布前认证其 support。
Host 内可进行 packed staging，但它不是第二种权威图像表示。

区分虚拟预留、已提供页 backing 和已产生有效样本。新提供的零页不意味着其中
所有像素有效。连续平面中一页可跨平面；页对齐分块模式中，一页不包含下一块，
但可以包含同一块的其他未请求样本。请求一个分量不计算或校验共享页面的其他
样本。缺失样本不隐式视为零。已发布区域保持不可变，后续可以产生同一图像
中其他不相交区域。

Structured Result coordinator 在 callback 读取前显式准备源 backing 和上游数据，
检查预算与取消。Private host windows 在使用期间保留页面；callback capability
限定在 Result Need 范围内。不能把内核调度和资源失败隐藏在同步缺页处理器中；
这不承诺操作系统不会发生普通按需缺页。

Public Result API 不暴露可无条件读取的整段预留地址 ByteView，也不提供独立
`PlanarImage` factory。图像读取要求有效 Result descriptor 和已发布 sample coverage。
Dense 导出须显式请求完整区域，raw 数值消费者也不能绕过这些条件。

资源统计区分虚拟预留字节、已提供页字节及 metadata。每个实际提供的页按完整
容量计费，包括共享该页的行／地址间隙，同时计算临时窗口和新旧 backing。
小 view 可保留多于逻辑 payload 的 backing，二者分别报告。页提供／commit
不等于跨平台物理 RSS 保证。地址预留上限和稀疏 bookkeeping 必须准入；不能为
巨大未使用范围的每个潜在页预分配一条 metadata。取消、上游与分配／供页失败会终结
受影响的 Result publication，未发布资源正常清理。

## 已选生命周期与失败策略

按需提供页，已产生页保留到图像退役。准入失败返回 ResourceExhausted，不驱逐
存活图像数据、不回放 producer，也不创建临时文件 backing。这里不保证物理页
锁定常驻；资源计数描述已提供 backing，操作系统驻留另计。活动窗口不可撤销。
失败且未发布的分配可正常释放，不能丢弃既有观察或共享页面中的有效数据。

CPU Result owner 和内部 planar backing 实现本策略。Packed ROI 读取是对 Result
image slot 的有界访问，不会创建另一语义图像对象或发布路径。

## 已验算的行填充与页对齐示例

W=200、H=130、C=4、Float32、T=128，页／预留粒度 P=16384。
首平面的 tile 如下，pitch 和偏移单位均为字节：

| 有效高 | 有效宽 | 行 pitch | tile 偏移 |
| --- | --- | --- | --- |
| 128 | 128 | 512 | 0 |
| 128 | 72 | 512 | 65536 |
| 2 | 128 | 512 | 131072 |
| 2 | 72 | 512 | 147456 |

下一平面从 163840 开始。四平面预留 655360 字节（640 KiB），包含有效样本
416000 字节、行 padding 116480 字节和地址间隙／尾部 122880 字节。块内存储段
合计 532480 字节（520 KiB）。R 的 (y=129,x=199) 偏移为 148252；没有已有 backing
时，请求该样本需要页索引 9 的一个 16384 字节页，逻辑读取仍只有 4 字节，不会
令同页其他像素有效。有效样本、虚拟范围与实际页 backing 必须分别计量。

## Result 拥有的图像存储与 public interfaces

Public contract 使用彼此独立的 package、workflow、traits 和 plugin versions：
package 0.29.0、WorkflowDocument schema 4、semantic operation traits 21、numeric C
operation ABI 11 和 Result operation ABI 1。这些版本不能互换。旧 planar C table、
planar operation callbacks 和独立 planar executor 已删除；不提供旧版本适配。

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
};

class ResultRef final {
 public:
  const SchemaTemplate& schema() const;
  Result<ResultDescriptor> descriptor(bool require_complete = true) const;
  Status read_image(const ResultDescriptor&, std::uint32_t slot,
                    const std::vector<std::uint64_t>& coordinate,
                    void* destination, std::size_t bytes) const;
};

class ResultBuilder final {
 public:
  Status publish_image(std::uint32_t slot, const Region& region,
                       ByteView packed, ResultRelation relation,
                       ResultFinality finality);
  Result<ResultRef> seal();
};
```

片段省略了 schema fields、budget/cancellation arguments 和其他 Result operations。
`SchemaTemplate` 可同时保存 image slots 与遵循同一 Result contract 的 primitive
fields。`ResultFieldSpec` 仍表示 packed primitive records；image samples 使用由 planar
pages backing 的 typed image slots。

`ResultRef` 是图像 schema、已发布 sample coverage、dependency relation 和生命周期的
public owner。`ResultRef::capture()` 在一个 revision snapshot 已认证的 descriptor、field/image
relations、descriptor basis 和 dependency bundle。Result 的有序 source association 是独立
live state；消费 inputs 时可单调扩展。Host actor 发布该 captured view，避免
observer 将早期 evidence 与较晚的 prefix 配对。Result resources 保留 schema 声明的 owners，
包括 typed image facets 引用的 ICC/OCIO resources；compiler 选择 nested resources，runtime
bindings 还会在 execution resource root 下重新准入。Public `PlanarImage` class 保留结构化的 `validate_layout` 检查和只读声明；
但创建、导入、view 组装、读写 acquisition、packed publication 和图像读取都是 private
实现方法，只供 `ResultBuilder` 与 `ResultRef` 访问。Valid backing 不会作为独立应用层
owner 返回。一个 Result 为每个 frame/layer pair 持有一份内部 planar backing。
`ResultBuilder` 在 Result budget 下创建 backing；最后一个 owning Result reference
释放其页面和虚拟地址预留。

内存计数分别表示该内部 backing 的虚拟预留字节、页 backing 字节、计费 metadata 和有效
samples。Private window/view 可能保留包含其逻辑读取集合之外 samples 的页面。Execution
root 按相应资源维度计量页面容量、metadata、staging、relation/maps、continuation state、
排队工作和保留的 owners。`resident_bytes()` 是计费快照，不是实测 process RSS。
Allocation、页面准备、取消或 callback 错误都不能为受影响 observation 发布成功。已发布
regions 保持不可变；重叠发布会被拒绝，最后一个 Result owner 释放 backing。

## 编译、执行与 CPU services

WorkflowDocument schema 4 通过 `WorkflowInputDeclaration.result_schema` 声明结构化输入；
`ExecutionBinding.result` 提供 owning `ResultRef`。具名 image output 是 Result output，
出现在 `ExecutionResult.results` 或 `DemandResult.results`。Numeric Values 继续使用普通非零 extents 与 affine tensor layout。Value storage 拒绝 structural image facet。

`PlanningOptions` 捕获具名 output regions 和正数的 2 次幂 tile extents。执行前，编译器解析
选定 output 和 image-slot schema。`ResultProgramQuery` 捕获 output index、image slot 和请求
footprint；tile height/width 决定物理 backing geometry。Image footprints 使用展平的
`{frame, layer, descriptor axes...}` samples，并按 Semantic Image 或 ColorArray tuple
contract 闭合到全部 channels。只有 TDM 的 facets 和 layout groups 不增加 sample support
或 alpha demand。

Operation registry 为结构化 outputs 选择 `start_result`。`ResultProgramNeed` 可同时请求
Result input 的 typed image samples、Value fragments、structured Result fields 和有界 host
I/O。Coordinator 会依据 source Result descriptor 和已发布 coverage 校验每个 Need。
Callback 经 `ResultImageInput` 和 `ResultProgramPhase::read_image` 读取；CPU parallel/tile
services 根据注册 operation contract 提供宿主计算能力。Callback 通过 `ResultBuilder`
发布 image regions 和精确 dependency evidence；不会收到 PlanarImage owner 或可写
PlanarImage window。C operation modules 使用 ABI 1 的
`result_operation_plugin_api.h` 提供相应 typed Result ports 和 services。Numeric C base
table 仍为 ABI 11。

Result execution 与 numeric Values 共用 plan 和 execution context。普通 execute/frozen 和
exact-demand paths 返回具名 Results；`execute_stream` 通过 Result observer 观察选中 Result
publication。Dependency relations 保留 Data、Control、Validation 和 Descriptor roles 的
selected sample support。已消费 Control sample 变化可使旧 relation 失效，并使下次 request
发现新的 Data samples。真正未消费的 control 或无关 tile 不产生 support。Typed Result runtime
descriptor observations 可通过 role bit 8 参与 dirty transpose。Numeric Value descriptor/facet
变化属于静态契约，需要重新编译；numeric Value dirty query 只接受 payload roles 1..7。改变已编译
contract 的 Result schema 变化也需要重新编译。更完整的 Result
和 dirty 契约见[全局结果](../../kernel-architecture/Global-Results.md)、
[依赖数据](../../kernel-architecture/Dependency-Data.md)和
[Region 语义](../../kernel-architecture/Region-Semantics.md)。

仍使用图像 Value contracts 的 production operations 不能通过 Result image slots 运行；
当前源码与 registry 状态见[图像 operations](../../kernel-architecture/zh/Image-Operations.zh.md)。

## 当前 fixture 源码

[`test_unified_result_images.cpp`](../../../tests/integration/test_unified_result_images.cpp)
使用测试定义的 operations 覆盖 Result image input/output、混合 numeric control 与 Result
ports、frame/layer samples、dynamic support 和 dirty replacement。
[`test_result_plugin.cpp`](../../../tests/integration/test_result_plugin.cpp) 加载 C11 Result
module。13 项 focused 检查全部通过，覆盖 8192 行分页、prefix publication、取消、共享执行、
captured facts、owner retirement、tuple closure、跨 frame support、dirty transpose、alias root-cache
复用、numeric multi-output 行为和有界 proof work。C11 Result fixture 与 native Metal Result fixture
也通过；后者执行一次 dispatch 和一次 submission，并回读到浮点值 4。Native GPU 检查还通过
affine 与 broadcast packed transfers，以及 500-unit 限额下的 root-work 拒绝。五个 installed
consumers 均通过：unified workflow、C++、Result contracts、C11 和 native GPU。C fixture 验证了消费
`ResultObjectNeed` 后绑定动态 Field domain，包括 Descriptor/Field 在零行和非零行之间替换。
`source_support()` 预算不足时返回带类型的 `ResourceExhausted`。

## 平台参考边界

[Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
区分 reserve 与 commit、主机页／分配粒度，以及 commitment 和物理分配。这支持
本文术语。CPU 存储实现按平台使用相应的虚拟地址预留和页供给机制；本文不声称所有
目标操作系统都通过了运行时验证。这些 CPU 地址公式不定义 native GPU 图像映射；
加速路径必须保留 Result slot 的所有权、sample 授权和发布契约。
