# Operation 与 Data-Definition ABI

Photospider 安装两份 narrow same-trust extension header：

- operation ABI v9：copied semantic trait、closed typed parameter schema、
  plan-derived input demand 与一个 synchronous Value callback；
- data-provider ABI v1：copied schema key、element type 与 maximum rank。

已安装的 C++ convenience wrapper `operation_plugin.hpp` 是可直接包含且 self-contained
的 header：它在暴露 `element_type_value` 前自行包含 `<cstdint>` dependency 与 operation
C ABI，不依赖 consumer 先包含另一份 Photospider header。导出的
`Photospider::operation_sdk` interface target 会传播 `cxx_std_17`，因此 C++ consumer
可以从 package 获得 wrapper 的实际语言要求，无需自行重复声明。Maintained consumer
通过与 compiler 相符的 dialect assertion 证明这项传播：MSVC-compatible frontend 定义
`_MSVC_LANG` 时使用该宏，否则使用 `__cplusplus`。它不私自添加 standard flag，也不
要求 `/Zc:__cplusplus`。

Provider contract 继续保持纯 C。`Photospider::data_provider_sdk` 只传播 include
directory，不传播 C++ compile feature，所以 C11 translation unit 可以消费它而不会
获得 C++ 语言要求。Package 不发布独立的 `data_definition_sdk` alias。

## Operation record

Operation descriptor 包含 length-framed key、input count、flag、estimated bytes、output
element type、closed scalar/preserve/match/fixed shape 与 Whole/Elementwise/Halo Region
rule、halo radius、cacheability、bounded parameter-schema pointer/count、callback 与
opaque plugin state。Parameter record 发布 unique key、精确 Int64/Float64/Bool/String
type 与 required presence。Compiler 在 semantic IR 前拒绝 unknown、missing、wrong-type
与 conflicting parameter；callback 只接收 validated canonical value，不存在隐藏 default
fallback。

Callback 接收 bounded dense whole-Region input view、每个 input 的 planned demand
offset/extent、bounded facet array、backend enum、cooperative cancellation callback 与
host-owned output sink。它最多发布一个带 bounded facet 的 output；host 复制并验证为
dense whole-Region Value。第一次 sink 调用即占用 publication，即使 validation 拒绝也不
例外。任何第二次调用都会设置 invocation-local sticky violation，且不分配、不抛异常、
不替换第一次 accepted/rejected `Result`；callback 返回后，adapter 会报告稳定的 terminal
`OperationFailed` diagnostic：
`operation plugin violated output sink at-most-once contract`。Null sink context 返回零，
且不改变 invocation state。DSO input view 精确覆盖其 logical contiguous bytes；trailing
backing bytes 会被拒绝，不能成为不可见的 plugin state。

Synchronous callback 保持 `int` signature，但返回一个闭合的 version-seven result：success、
ordinary failure、cancellation 或 backend unavailable。backend unavailable 与 ordinary
failure 不同，并且只有 copied trait 允许时才能从 GPU attempt 请求 CPU fallback。unknown
nonzero integer 是 ordinary `OperationFailed` result。报告 backend unavailable 的 callback
不得调用 output sink。若已调用，accepted output 是 terminal `OperationFailed` contract
violation；rejected generic output 保留 sink 的精确 typed failure。错误 image output
返回 OperationFailed，显式 callback cancellation 与 resource exhaustion 保留各自分类。两种路径都不暴露
`BackendUnavailable` 或触发 CPU fallback；host cancellation 继续是最高优先级 result。
在该 cancellation check 之后，duplicate sink violation 优先于 success、backend
unavailable、ordinary failure、callback-reported cancellation 与 unknown result；因此它
绝不会发布第一次 Value，也绝不会请求 CPU fallback。

在任何 C++ 或 DSO callback entry 之前，`OperationRegistry::invoke` 会验证 operation/
input/demand count，先检查每个 input 的 `Value::valid()` 再读取 descriptor，验证每个
demand 与 parameter，观察 host cancellation，拒绝 CPU/GPU 之外的 backend value，随后
检查 backend capability。已知但不支持的 backend 仍为 `BackendUnavailable`；未知数字
backend 返回 `InvalidArgument`，且 DSO adapter 绝不会把它转换为 GPU。完成这些更高
优先级检查后，registry 使用 `resolve_operation_traits` 和 `infer_operation_output`，
与 semantic lowering 共享 dtype、shape、canonical output facets 推断。Preserve/Match
仅比较 shape，input dtype 限制由端口显式声明；失配在 callback 前拒绝。
Callback output validation 复用该预计算
descriptor；成功 callback 返回 default-invalid generic `Value` 时仍安全地得到
`TypeMismatch`，错误 image output 返回 `OperationFailed`。
直接调用与物理规划共享受检查的输入需求规则。Registry 在 callback 前拒绝 Value/halo
覆盖不足、不完整通道图像输出和蒙版 shape 不匹配。计算产生的蒙版数值错误为
OperationFailed，绑定蒙版数值错误仍为 InvalidArgument。

C++ `OperationTraits::Fixed` record 只描述 logical output descriptor。Registration 会
验证非零 rank-1..8 shape、闭合 element type/rule 与普通 trait combination，但不会计算
dense element/byte product。Callback 可返回任何通过普通 publication validation 的 Value
layout，包括在巨大 logical shape 上只占八字节的 zero-stride broadcast。
`estimated_bytes` 是独立的 modeled admission estimate。同步 C DSO Fixed descriptor 更严格，
因为其 sink 不携带 output stride：loading 会独立要求 contiguous signed stride 与 uint64
byte count 可表示。对于 dense total bytes `B`，loader 还要求 `B > 0`、zero-based last
byte `B - 1 <= INT64_MAX`，以及 `B <= SIZE_MAX`。因此在 64-bit host 上，UInt8
`{INT64_MAX + 1}` descriptor 与 `{2, 2^62}` 可表示；任一边界再增加一个 element 都会被
拒绝。复制的 `requires_dense_output` trait 还会在 semantic IR 发布前按解析后的 dtype
检查完整输出，覆盖 dtype 来自输入或静态参数的 Fixed 输出。该要求为 false 时，
C++ Fixed broadcast 语义继续有效。分阶段 C dependency program 也将该字段设为 false：
宿主服务逐个校验实际输出 fragment。仅物化有界合法片段时，logical Fixed domain
无需具有可表示的完整 dense byte product。分阶段 C 契约见
[依赖数据](Dependency-Data.zh.md#c-分阶段程序)。

## Validation

在任何 Windows、Linux 或 Darwin native-loader 调用前，operation/provider loading 会把
精确 `std::string` path 验证为非空、最多 4096 bytes 且不含 embedded NUL。Malformed path
返回 `InvalidArgument`；平台无法加载的合法精确 path 仍返回 `NotFound`。被拒 path 不会
打开 truncated prefix，不发布 registry key/schema，也不会启动 native-owner lifecycle。

Loading 验证 exact ABI version/structure size、pointer/array alignment、pointer/count pair、
bounded key/count/rank/parameter value、严格 UTF-8 operation/parameter-schema/
provider-schema key、duplicate parameter declaration、closed enum/flag/type combination、
required callback、logical C++ fixed descriptor、dense C DSO fixed stride/byte
representability（包括 signed last-byte 与 host allocation-size bound）、output
element/shape/byte count、facet array/key/version/payload、arithmetic overflow 与
exactly-once destroy ownership。Key validation 会在 publication
前拒绝 invalid continuation byte、truncation、overlong encoding、UTF-16 surrogate、
大于 U+10FFFF 的值、embedded null 与 ASCII control，但不执行 Unicode normalization。
普通 facet payload 与 Value byte 仍是 opaque binary data。该 ABI version 拒绝 trailing
structure bytes，也不发布 v1 compatibility entry point。

Malformed registration 不发布任何内容。Builtin/embedding definition 与每个 DSO
definition 都会在 publication 前完整构造，随后由 private immutable owning handle 保留。
Registry map、DSO transaction staging 与 invocation snapshot 只复制该 handle：registry
mutex 持有期间绝不复制或执行 embedding callable。Multi-record publication 对 handle map
执行 copy-then-swap，因此 allocation failure 不能暴露 prefix；被替换的 map 在 unlock 后
retire。Invocation 持有的 handle 会让 callback 及其捕获的 DSO lease 存活到 callback
完成。Embedding C++ operation callback 的 `std::bad_alloc` 会继续传播，使 caller 能够
保留 resource-exhaustion policy；其他每个 `std::exception` 都映射为
`OperationFailed`。若 `what()` 返回 null，则在不从 null 构造 string 的前提下规范化为
空 diagnostic。Nonstandard exception 获得稳定的通用 `OperationFailed` diagnostic。
Output 在 callback 返回前复制。Plugin-owned descriptor table 在 library unload 前 destroy。

Dense layout product 在 multiplication 前使用 checked uint64 division，随后验证 complete
byte range。Boundary fixture 会加载真实 DSO，并要求后续 descriptor 不可表示时进行
transactional rejection，同时 destroy 与 native close 各恰好一次。Compile-time-width
helper 即使在 64-bit test builder 上也会实例化 32-bit allocation-size path。

Native open 后，每个 operation/provider handle 都立即由 move-only stack owner 持有。
当 exact API structure prefix 可安全读取后，该 owner 也接管已经取得的 destroy callback。
因此 symbol、table、schema、heap-owner 或后续 staging failure 会对每个已取得的 destroy
callback 与 native close 各调用恰好一次。成功 loading 会把同一个 owner 显式 move 到
published heap lease。

Provider registry 在 copied schema record 之前声明 native lease，因此逆序析构会在
最终 provider destroy callback 与 native unload 前退役所有 registry-owned schema。
`find()` 结果自行拥有复制后的 key，也不包含 DSO pointer，因此可在 registry teardown
后继续存在而不借用 mapped provider memory。

## Lifecycle 与边界

Path 只来自 embedding-process startup configuration，并作为精确且 NUL-free 的 byte
sequence 消费。Registry 在 compiler/executor 使用前完成 assembly 并 freeze。DSO 与 host
在同一 trust domain 内执行。ABI check 是 correctness validation，不是 sandbox、signature、
certificate、package admission 或 process isolation。

不存在 policy ABI/SDK/DSO、external scheduling plugin 或 IPC plugin path。Data-definition
ABI 不构造 Value，也不提供 storage。

## Version-nine 语义与输出契约

Package 0.9.0、operation ABI/traits 9 替换 0.8/8。Host 先检查 version，再读取
`get_api_v9`；无旧 table、symbol alias 或 image-v1 reader。WorkflowDocument schema 2、
provider ABI 1、C++17 保持。

`data/semantic.hpp` 的 `SemanticDescriptor` 编码 image-v2/semantic-v1 facet，canonical
payload 上限 4096 bytes。Helper 构造 RGBA/coverage、验证元数据/区域样本，以及转换
最多 8192 字符的 lowercase-hex 静态 `semantic` 参数，调用者不必手写 hex。通道名/角色/
单位、color/white/transfer/reference/association、采样轴与样本值单位分别声明。
Image 为 Float32 HWC，typed image validation 支持 finite signed/HDR。Vector token
区分 pixel/normalized displacement/position；complex 固定完整未 shift 频谱、DC0、
负号无归一化 forward 和 inverse /N。仅描述数据，不实现 FFT。Generic opaque facets
及其浮点位模式保持；构造 Value 元数据时拒绝 malformed known typed facets。

每个 C port 可指向 exact-sized semantic constraint，声明 kind/facets/dtype/rank。
`element_type_mask` 低 0..3 位对应 UInt8/Int64/Float64/Float32，零表示无限制，
与非零精确 `element_type` 互斥；未知位或冲突字段在注册时拒绝。复制的 mask 进入
compiler/result identity。
每输出 contract 选择声明/输入/静态参数 dtype，rank 1..8 axes（常量、正 Int64
参数、输入轴、实际输入数量）及 checked 非负偏移，output semantics 选择 drop、preserve
input、establish facets 或静态 semantic 参数。固定前缀后可有一个同构重复组，启用时
minimum>=1、maximum 有界，总输入<=1024。Loader 在原子发布前复制全部 record，lowering
展开精确有序表。每输出 Region 与分阶段依赖协议决定空间读取；Whole 保留保守需求。

闭集 semantic 词汇还会根据输入元数据推断通道提取/选择/合并、alpha 关联和 RGB/XYZ/Lab
变换。`IndexListCount` 与 swizzle 共享公开 canonical indices parser：1..64 个 [0,63]
十进制索引，逗号分隔，无空格或前导零。规则复用既有 source/parameter 字段，发布前
拒绝非法组合，不按 operation key 分派推断。精确 role、参考白、去 alpha 和 generic
输出行为见[通道与颜色算子](Channel-and-Color-Operations.zh.md)。

闭集 `SampleExpression`/`ApplyLut1d` 在编译器、直接调用及 C 声明中共享有界 parser
与均匀域校验。前者输入 generic Float64 `[K]` (1..256)，要求有限 start、有限正 step，
验证已解析 Float32 `[count]` (1..1048576)，输出值/轴单位 dimensionless；多样本端点
有限且大于 start。后者接受 SampledSignal query 及 N>=2 的 SampledSignal/Lut 表，
query 值单位匹配 table 轴单位，输出 Drop。两者 Whole，见
[表达式与 LUT](Expression-and-LUT-Operations.zh.md)。

SemanticNode/PlanStep 保留真实 output facets，C sink 向 callback 提供相同的已解析
类型/shape/facets，并据此检查结果；typed facet 失配为 OperationFailed。Drop 移除已知
typed 语义保证；注册时拒绝 Drop（包含 NULL C contract 的默认规则）搭配
RgbaFloat32、Float32Mask 或 Typed 输出端口。此类输出必须显式选择 preserve、
establish 或 transform；端口种类本身不建立语义。无关 opaque generic facet
保持既有发布规则。规划与 tile 派生从推导后的 output facets 识别 image，要求完整
逻辑 C 通道覆盖，包含 generic 端口和 Whole 输出。RGB/XYZ/Lab 的三或四通道图像
仍允许 HW 空间 Region。直接调用在 callback 前应用同一通道覆盖检查；执行、
frozen Region 与 stream 继承规划的覆盖要求。完整约束及输出规则进入
v8 compiler identity 和 v4 result-region key。

共享契约与八个既有算子现已支持 image-v2 signed/HDR RGB、canonical
coverage-premultiplied D65 语义，包含符合资格的原生 Metal 执行。八算子显式保留
首输入的语义 facet。快照与 memory/native/disk cache 保留受支持 image-v2 表示和真实
canonical facet；磁盘格式 2 拒绝旧格式。Bounded scalar 接受兼容 computed Float32 `{1}`，包含 dimensionless Scalar/单样本 Signal facet。
每个 consumer 在 callback 前检查范围（含缓存命中），直接绑定保留 preflight。标量读取
使用逻辑地址，支持 padding/stride。默认 registry 也提供 CPU Whole [数值算子](Numeric-Operations.zh.md)。
通用 typed image validator 也接受 straight 表示；八个既有算子端口要求 canonical RGBA。

## ABI 9 区域视图与宿主分配

ABI 9 输入包含独立 storage origin、byte offset、signed strides、有效 coverage 和 demand。
输出 sink 提供精确 descriptor/Region 和 packed 字节数，allocate_output 返回宿主输出，
allocate_scratch 返回回调局部临时缓冲区。发布宿主输出直接冻结；发布栈/调用方数据则
通过相同宿主分配器复制。指针只在回调期间有效，禁止自行释放或保留。通用输入允许
backing padding，只能按 origin/stride 访问有效区域。首个发布即占用 sink，重复发布
不能替换结果并返回 OperationFailed，宿主取消优先；资源失败保留分类。旧整图 dense
输入/复制输出说明由本节替换，宿主在 API table 查询前拒绝 ABI 7。

## S3 缩放端口

ABI 9 包含 S3 引入的 Shrink 形状/区域规则及必需的 spatial_factor_parameter 指针/长度。
有界 Int64 参数解析为 [1,16]，输出 H/W 向上取整，输入需求为裁剪 box。允许蒙版
输出。未知布局、指针/数量失配、非法范围和旧 ABI 7 在发布前拒绝。

## S4 宿主 GPU 服务

ABI 9 输出 sink 的 gpu 指向调用内 ps_gpu_service_v9，CPU 时为空。buffer 创建宿主
分配的有界 token，禁止写入已冻结输入；execute 验证 shader、entry、bindings、常量及
grid，完成后才返回。参数失败粘滞，不能由 callback 成功覆盖。Token/指针不跨回调
保留，提交后错误终止 Run；无发布的数值/后端拒绝允许按 trait 回退。CPU 与 C 模块
使用同一服务，SDK 不暴露 Objective-C 类型。详见公开头及 S4-Workflow。

## G4 观察与 continuation 契约

ABI/Traits 9 增加 Atomic、终端 RequestRecord 与显式 RequestFailureOnly。当前 C
descriptor 复制并校验这些字段，同步调用继续可用。`dependency_plugin_api.h`
提供可选 C 分阶段程序表，使用宿主 continuation、精确关联、fragment 读取和跨 poll
owner handle。ABI 9 增加可选 `ps_dependency_joint_program_v9`，使用相同服务与完成
规则验证各成员 outcome，详见 Dependency-Data 的 M4 章节。

C++ registry 可选择 `start_dependency`，通过有界宿主 continuation、poll 和 supply
阶段执行。编译器检查全部输入祖先的 EffectiveAtomic，拒绝 RequestRecord 全部出边。
协议、allocator 生命周期和 CPU Run 行为见 [依赖数据与执行](Dependency-Data.zh.md)。

## ABI 9 输出选择与联合执行

`OperationTraits::outputs` 为非空有序 `OperationOutputTraits` 表，上限 64。
每输出独立声明唯一端口名、schema、shape、facets、输入投影、Region、观察和失败
契约；单输出内建声明 `value`。C ABI 内嵌有界输出表与数量。Shape axis 支持检查
溢出的 ceil-div、参数减法和用于非整数 radius 的 `CeilParameter` 缩放。
多输出算子必须确定且无外部副作用。

Invocation/query/sink 携带原始 `output_index`；投影输入 view 保留原始
`input_index`，不填 invalid Value。推导仍可获取完整静态输入元数据。
C/C++ joint continuation 可选地处理每输出一个 Atomic 观察。服务、coverage、
handle、读取关联和终态错误均按成员隔离；宿主拒绝重复、缺失、未知成员和过期或
跨成员 handle。共享 work 服务只计费一次共同计算，错误具有粘性。
Singleton 入口仍必需；RequestRecord 不参与联合执行。Provider ABI 保持 1。
调度、资源、缓存和数值契约见 ADR 0021 与多输出算子指南。
