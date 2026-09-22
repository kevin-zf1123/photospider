---
spec_schema_version: 1
id: KERNEL-tensor-storage-zh
status: Accepted
implementation_status: implemented_cpu
clarification_status: selected_storage_policy_complete
---

# 张量存储与区域访问

本文是 package 0.19.0 所实现的存储契约译文，源于 2026-09-22 的 FMT 澄清，
[英文版本](../Tensor-Storage-and-Region-Access.md)为权威。本文
负责物理布局、tile 几何、区域访问和存储所有权；颜色与 alpha 解释由
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义。下文明确 CPU 接口支持范围与迁移边界；本次不实现 FMT-01，也不保留
已退休图像的内存或数值契约。

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

tile 大小是 DAG 策略，不是算子参数。128×128 默认值保留且可配置，不
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

CPU 存储 owner 和窗口 API 实现本策略。FMT-01 仍为独立的 Proposed 算子族，
采用 auto/view/materialize；其物化结果须在完整结果虚拟范围内提供所请求样本。
紧密 ROI 读取属于显式窗口操作，不是第二套权威图像表示。

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

## 已实现的 CPU 接口与迁移边界

[PlanarImage](../../../include/photospider/data/planar_image.hpp) 是通用
`ValueDescriptor`、facets、resources 和结构轴／分组的物理存储 owner，不增加
RGB、alpha 范围、预乘或颜色 transfer 运算。rank-2 声明高／宽轴且没有通道轴；
rank-3 显式声明全部三个轴。当前物理 dtype 为 UInt8、Int64、Float32、Float64。
分量组采用互不重叠的通道区间，最多 64 组；role 非空且最多 128 字节。

`PlanarImage::create` 预留整图，但不使样本有效。`import_value` 显式将完整的交错／
strided 外部 Value 复制到声明的 planar 布局，不另建整图 packed 缓冲区。
`publish` 以事务方式复制精确 packed 区域；`acquire` 返回保留 owner 的读取窗口，
`row_run` 在获准 ROI 或 tile 边界停止。`read` 是显式 packed 区域导出。
没有接口将完整预留地址暴露为可无条件读取的 ByteView。缺失覆盖返回 NotFound；
重复发布既有样本失败，不修改已发布区域。

已准备的 `PlanarImageWriteWindow` 只提供获准行段。宿主在调用算子前准备目标页，
成功完成后提交覆盖。放弃窗口、回调失败或已观察到的取消，会回滚未发布页和计费，
保留既有已发布区域。普通读取窗口不阻止不相交发布；获取锁时观察取消。
执行还会固定外部输入 owner，在 Run 退役前以 Stale 拒绝发布，使准入期间输入容量和样本
覆盖保持稳定。

统计区分 `reserved_bytes()`、`backed_bytes()`、`metadata_bytes()` 和
`valid_samples()`。metadata 保守计费，包含持有的 groups／facets、稀疏覆盖／页记录
及事务峰值容量。`resident_bytes()` 是 backing 加已计费 metadata 的原子快照，
**不是**实测物理 RSS。`PlanarPageBudget` 聚合各 owner 的 backing 与 metadata，
执行时租约接入通用张量使用的同一个 `MemoryBudget`。重复 owner 以及回绑到原计费域
的结果不能重复计费。逐图虚拟地址、页面、metadata 记录和访问工作量分别检查上限。

### 公开编译与执行链路

WorkflowDocument schema 3 使用 `WorkflowInputDeclaration.planar_layout` 声明图像，
其仿射 `layout` 必须为空。声明包含存储模式、空间／通道轴、行 pitch 和分量组。
tile 几何统一来自 `PlanningOptions`，整个 DAG 默认 128×128。通用数值声明继续使用
其仿射布局；raw metadata override 不能交换这两种存储契约。

C++ 算子显式注册 `planar_storage_capable`、`planar_callback` 和
`OperationOutputTraits.planar_layout`。编译器检查边上的结构布局连续性。
回调接收精确读取窗口与宿主准备的写窗口，不接收可任意写入的输出 owner。
执行器验证绑定与声明／DAG tile 几何一致，使用共享 CPU 队列与准入服务，并在回调
执行前及结果发布前检查取消和图版本。具名图像结果位于 `ExecutionResult.images`。
通用 `values` map 不隐式导出 dense 图像；调用方显式 acquire 或 read 图像区域。

当前 planar 算子路径支持至少一个 planar 输入、通用 `Value` 端口 schema、CPU 单输出回调
及 Whole／Elementwise 区域规则。数值检查由消费算子负责；该路径不接受旧 `Typed`
或特殊 image／mask／scalar 端口 schema。
不支持的回调／trait 组合在注册时拒绝，包括 GPU、staged／joint 执行、prepared
metadata specialization、workspace 声明及旧 view-output traits。尚无结构图像输出
支持的 freeze／demand／stream／atom 入口明确拒绝请求。旧 image／Layer 结构化
结果 schema 也不属于本存储契约，不能绕过 Value 门槛。不回退到旧图像 Value、
独立分块 snapshot 或原有数值规则。这些是后续算子迁移的明确能力边界。
非图像通用张量能力继续提供。

### 可运行验收

[公开 workflow fixture](../../../tests/integration/test_planar_image_workflow.cpp)
注册 planar copy 回调，构造两节点 WorkflowDocument，并通过 Compiler 和
ExecutionContext 执行。ROI 跨越四个存储 tile，oracle 检查原始样本位、精确有效
覆盖和缺失样本失败。其他案例覆盖连续平面、不同轴顺序、边缘 padding、小块页对齐、
owner／窗口生命周期、资源耗尽、回滚、取消和同 context 结果回绑。预期地址使用实际
主机页几何；上面的 16384 字节页示例不是平台默认值。安装消费方用安装后的包构建
同一公开 fixture，不依赖内核私有头文件。

构建前遵循仓库的[依赖前提](../../development/Testing-and-Validation.md#build-prerequisite)。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target test_planar_image_workflow -j 8
ctest --test-dir build -R '^test_planar_image_workflow$' --output-on-failure
ctest --test-dir build -R '^test_installed_consumer$' --output-on-failure
```

planar fixture 检查 DAG 结果和存储边界后以状态零退出。包消费 gate 还保留通用张量、
C SDK 和共享库消费检查。已退休的旧图像 golden tests 不构成新接口的支持证据。

## 平台参考边界

[Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
区分 reserve 与 commit、主机页／分配粒度，以及 commitment 和物理分配。这支持
本文术语。CPU 后端在 POSIX 使用匿名虚拟地址预留及显式页保护／供页，在 Windows
使用 reserve／commit。本机 macOS 的 getconf PAGESIZE 返回 P=16384，后端读取实际
主机属性，不固定假设 4096。macOS 原生验收不代表 Windows／Linux 运行时已验收；
planar 回调路径尚不提供 GPU 图像映射。
