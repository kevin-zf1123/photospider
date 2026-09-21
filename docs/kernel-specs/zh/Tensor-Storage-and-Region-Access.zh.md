---
spec_schema_version: 1
id: KERNEL-tensor-storage-zh
status: Proposed
implementation_status: not_implemented
clarification_status: selected_storage_policy_complete
---

# 张量存储与区域访问

本文是 2026-09-22 随 FMT 澄清的目标规格译文，
[英文版本](../Tensor-Storage-and-Region-Access.md)为权威。它描述尚未实现的目标，
负责物理布局、tile 几何、区域访问和存储所有权；颜色与 alpha 解释由
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义。本规格不创建 ADR，不改变运行时。

## 已确认决定

| 主题 | 目标 |
| --- | --- |
| 图像布局 | 所有图像必须 planar；交错图像导入时显式转换布局。 |
| DAG tile 策略 | 同一 DAG 统一 tile 尺寸，各算子不能独立决定输出 tile 尺寸。64×64、128×128、256×256 是示例，不是已确定的完整尺寸白名单。 |
| 平面组织 | 保留连续平面模式；采用 tile 时，所有分块图像平面服从 DAG 配置，不提供逐平面尺寸覆盖。 |
| backing | 预留整图连续虚拟地址范围，按需提供页 backing；平面／tile 访问共享图像地址空间 owner，不另建互不相关的图像分配。 |
| 行 | 平面／tile 的行内样本连续，允许行尾 padding。 |
| 颜色与 alpha | 同一张量可包含颜色组和独立 alpha 平面，例如 L*、a*、b*、Alpha；alpha 不属于 Lab 颜色坐标。 |
| 边缘 tile | 只保留有效行，每行宽度补齐到统一 tile 宽度；中间块 payload 为紧密完整块。下一 tile 起点强制页对齐，地址间隙与行 padding 分开；不补缺少的底部行。 |
| 页准备 | 算子执行前显式取得、准备所需页，检查资源与取消；不在缺页异常中调度 DAG 计算。 |
| 页保留 | 已产生数据的页 backing 保留到图像最后一个生命周期 owner 释放；超预算失败，不自动驱逐、DAG 回算或临时文件换页。 |

DAG 统一配置替换此前逐平面／逐算子 tile 配置建议；整图连续虚拟地址替换独立
分块 owner 建议，实际页 backing 按需提供。这些要求不恢复 Image/Layer 特殊语义类型。张量 shape/dtype、
结构布局、颜色组与实际消费的 metadata 继续分离。raw／override 不能改变真实
地址映射，也不能通过改标签把交错图像变成平面图像。

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
语义；本契约不定义预乘 Lab，也不对 alpha 执行颜色 transfer。算子声明所消费的
组／分量，共享 backing 不自动增加完整颜色或 alpha 的校验／读取义务。

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

tile 大小是 DAG 策略，不是算子参数。当前 128×128 默认值是保留默认的候选，不
限制为唯一尺寸。halo 与跨 tile ROI 可以超过一块，但不改变输出块几何。无图像
空间轴的普通数值张量不凭空增加图像轴。不合规的图像存储通过显式导入／布局
转换满足 planar 和 DAG tile 配置。

## backing、view 与精确访问

连续指具有共享生命周期的整图预留虚拟字节范围，包含对齐间隙，不要求物理 RAM
页相邻，也不要求一次提供整图 backing。每个坐标在范围内有稳定偏移。平面／tile
view 保留地址空间 owner 和访问窗口所需 backing；owner 存活期间保留完整虚拟
范围及已产生数据的全部页。关闭一个小窗口不会驱逐仍存活图像的页；最后一个
生命周期 owner 释放页和地址范围。

请求指定精确逻辑覆盖与分量，内核映射到对应 tile 局部，返回受支持的 view／分片
或显式物化紧密读取窗口，跨块不要求收集整图。物理传输／页粒度可能超过逻辑请求，
分别报告和计费。部分图像输出写入预留整图范围内的相应偏移。独立紧密读取窗口
可作为显式临时存储，但不成为该图像的权威主存储。

区分虚拟预留、已提供页 backing 和已产生有效样本。新提供的零页不意味着其中
所有像素有效。连续平面中一页可跨平面；页对齐分块模式中，一页不包含下一块，
但可以包含同一块的其他未请求样本。请求一个分量不计算或校验共享页面的其他
样本。缺失样本不隐式视为零。已发布区域保持不可变，后续可以产生同一图像
中其他不相交区域。

执行器在算子访问前显式准备源／目标页及上游数据，检查预算与取消。算子获得
有界访问窗口，窗口存活期间保留所需页，不能撤销仍使用中的窗口。不能把内核
调度和资源失败隐藏在同步缺页处理器中；这不承诺操作系统不会发生普通按需缺页。

不能把整段预留地址暴露为可无条件读取的 ByteView。访问同时需要已提供 backing
和获准的有效样本覆盖；dense 导出须显式取得完整所需区域，raw 数值消费者也不
绕过这些条件。

资源统计区分虚拟预留字节、已提供页字节及 metadata。每个实际提供的页按完整
容量计费，包括共享该页的行／地址间隙，同时计算临时窗口和新旧 backing。
小 view 可保留多于逻辑 payload 的 backing，二者分别报告。页提供／commit
不等于跨平台物理 RSS 保证。地址预留上限和稀疏 bookkeeping 必须准入；不能为
巨大未使用范围的每个潜在页预分配一条 metadata。取消、上游与分配／供页失败
不发布受影响观察的成功结果，未发布资源正常清理。

## 已选生命周期与失败策略

按需提供页，已产生页保留到图像退役。准入失败返回 ResourceExhausted，不驱逐
存活图像数据、不回放 producer，也不创建临时文件 backing。这里不保证物理页
锁定常驻；资源计数描述已提供 backing，操作系统驻留另计。活动窗口不可撤销。
失败且未发布的分配可正常释放，不能丢弃既有观察或共享页面中的有效数据。

本层存储策略已确定。平台预留／供页调用、能力报告、metadata 编码、并发控制和
公开窗口 API 仍需实现契约与验证。FMT-01 使用 auto/view/materialize：materialize
在新结果的完整虚拟范围内提供所请求样本；紧密 ROI 读取属于显式窗口操作。

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

本次文档会话通过独立整数地址枚举检查了 104000 个样本起点的唯一性和边界，
并比较顺序 tile 前缀累加与上述闭式偏移公式，核验总预留量和单样本页面计算。
另检查完整 64×64 UInt8 tile：4096 字节 payload 后，下一块仍从 16384 字节页
边界开始。未分配虚拟图像、未运行算子，也未测量 OS 驻留。

## 当前实现与迁移条件

当前 [Value](../../kernel-architecture/Data-Model.md) 使用一个仿射布局和 owner。
[ValueFragments](../../../include/photospider/data/value_fragments.hpp) 支持精确矩形，
但已识别的图像／颜色元组仍限制部分通道分片。当前
[快照存储](../../../src/lib/data/input_snapshot.cpp) 独立分配各块，并把颜色元组的
通道放在一起，尚不满足新的整图连续 planar 目标。快照块尺寸与
[规划器 tile 配置](../../../include/photospider/compiler/compiler.hpp)也是独立机制，
需要统一 DAG 策略。

迁移必须交付结构性图像布局描述、虚拟地址预留与页 backing、区域读写窗口、行填充边缘布局、
metadata 组投影、显式导入转换，以及编译器／provider 对 DAG tile 策略的检查。
在实现与公开工作流验证完成前，现有实现文档继续描述当前事实。

验收应独立检查连续／分块平面寻址、非整除尺寸、行／页 padding、同 DAG 几何一致、
颜色／alpha 分组、跨块精确分量读取、不可变生命周期，以及虚拟预留／供页／有效样本量。
还需验证页面混合已产生／未产生区域不会伪造有效性，活动窗口可用，稀疏 ROI 不提供
整图 backing，失败时清理部分供页。
检查 padding 不参与样本或边界扩展，交错图像未经显式转换不能作为合规图像绑定。
本轮未实现，也未运行算子测试。

## 平台参考边界

[Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
区分 reserve 与 commit、主机页／分配粒度，以及 commitment 和物理分配。这支持
本文术语，不代表跨平台后端已实现。本机 getconf PAGESIZE 返回 P=16384；布局需
读取主机属性，不固定假设 4096。POSIX/macOS、Windows 及 GPU 映射路径需要各自
的实现与验证证据。
