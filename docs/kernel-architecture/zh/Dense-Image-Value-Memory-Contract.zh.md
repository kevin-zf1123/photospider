# 稠密图像 Value 内存契约

本文档定义普通稠密图像当前的内存、metadata、发布和外部 artifact 契约。一个稠密图像就是一个
内置 `Value`：它具备 `DenseTensorDescriptor`、完整 `ImageFacet`、显式 Layout、一个或多个
受检 `BufferHandle` binding 以及一个 `ReadyFence`。不存在第二种图像值、兼容 snapshot、
存储枚举或 native-context carrier。

[Data Model](../Data-Model.md) 中的通用契约仍是权威来源。本文档将其专门化到普通图像及其
Host、codec、cache、IPC、worker 和 durable 边界。OpenEXR Deep 仍是独立的 provider-defined
variable-sample 契约。

## 逻辑 descriptor 与图像解释

`DenseTensorDescriptor` 拥有 rank、正的具体 shape、`ElementSemantics`、`StorageEncoding`
和可选 quantization。它不拥有坐标、物理 stride、device placement、readiness、sample 含义、
color 含义或观测 statistics。

每个普通图像都有一个 `ImageFacet`，其中 x/y axis 显式且互异，并可带一个同样互异的 channel
axis。在当前内置 image view 中，未分配给 x、y 或 channel 的 axis 必须为 singleton。该 Facet
包含：

- 必需的 signed half-open data window，其 span 与 descriptor 的 x/y extent 精确相等；
- 可选且独立的 signed half-open display window；
- 可选的稳定 channel/group schema；
- 可选的版本化 sample encoding/domain 声明；
- 可选的版本化 color 解释。

data window 是逻辑 pixel-coordinate 权威。display window 是 presentation metadata，不授予
payload 访问。`ImageView` 同时提供 zero-based storage coordinate 和受检的 signed logical
coordinate；逻辑访问仅在完成 containment 检查后才减去 data-window origin。

诊断性的 channel/group 名称从不选择 semantic role，也不进入 semantic equality。稳定的
`ChannelId` 和 `ChannelGroupId` record 承担该职责。观测 extrema、histogram、NaN/Inf count
及其他 statistics 永远不会成为 descriptor、Facet 或 content identity。

## Layout、binding 与所有权

普通内置图像使用经过验证的 whole-byte `StridedLayout`。descriptor shape 和 element width
与 byte stride/offset 相互独立。immutable Value 可以保留受检的正、零或负 stride；可写 producer
builder 则要求 non-overlapping positive layout 和精确的 storage envelope。

`BufferHandle` 是一个受检 immutable byte range，位于一个显式 `StorageBinding` 上。它不暴露
raw pointer 或任意 context payload。CPU binding 会保留 selected range 所保证的正二次幂
alignment；checked subrange 会在 offset 无法维持原保证时降低该值。alignment 是 physical
reconstruction fact，绝不是 descriptor 或 content identity。Host 只能通过 retaining read lease 或独占 producer/grant
write lease 访问。source-private device Value 保留显式 `DeviceBackend`、`DeviceId`、
`MemoryDomain`、native owner 和 byte range；device-to-Host transfer 会创建不同的物理 binding，
但不改变逻辑 descriptor。

`ValueBuilder` 在唯一 write lease 被释放且 `seal()` 成功前拥有 mutable CPU storage。seal 会在
发布具有全新 immutable Value revision 的结果前验证 descriptor、Facet、Layout、binding、
storage envelope 和 producer state。`PendingValuePublisher` 与 device publication 遵循相同的
single-authority 规则：terminal failure 会清除 private producer access，成功的 Ready publication
之后不可再修改。

## Readiness 与 metadata-only inspection

readiness 属于 `ReadyFence`，而不属于图像 descriptor 或 artifact。Pending、Ready、Failed 和
ProducerCancelled 是封闭状态。payload view、buffer lease、codec read 和 content digest 计算
要求 Ready。

Host metadata inspection 会复制 descriptor、Facet、Layout summary、buffer envelope、readiness
snapshot、producer identity、可选 canonical digest 以及有界且 identity-independent 的 statistics
reference。它不会取得 payload lease、map device、等待 fence、计算 digest 或调度 statistics。
因此 non-Ready output 仍可 inspection，同时不会放宽 payload 访问规则。

## 命名 Host output

Embedded 和 IPC Host 返回 canonical ordered named Value。output name 有界、非空、唯一并已排序；
`image` 是普通图像 output 的约定名称，但不是独立 result type。成功 result delivery 要求每个已
声明 output 都存在且 Ready。failure 拥有一个有界 `OperationStatus`，并且不发布 partial Value set。

formal、dirty、tiled 与 real-time 路径发布相同形态的 named Value。dirty execution 可以从 signed
data window 派生受检、storage-relative 的 `PixelRect` work，但 commit 保留原始 logical window
和 Value authority。resize/downsample scratch Value 仅属于 callback-local，永远不会作为替代
snapshot 进入 formal cache state。

## 可移植 Value artifact

外部和 durable 边界使用 `ValueArtifactEnvelope` version 1 与 canonical named artifact set。
envelope 保留完整内置 descriptor、ImageFacet、Layout、有序 buffer role/span/alignment、payload
digest、可选 content digest 和有界 statistics reference。store owner 用自身的 artifact、commit、
slot、attempt、lease、quota 与 path authority 包装它；这些事实不会成为 Value identity。
capture 会记录每个 CPU binding 所保证的 alignment。version 1 接受不超过 4096 byte 的正二次幂
要求，分别对齐 archive 中的每个 span，并按对应的精确要求重建每个 Strided、Blocked 或
provider-defined buffer。
finite binary32/binary64 metadata 会从数值的 IEEE-754 sign、exponent 与 fraction 编码为 canonical
little-endian word，而不依赖 native object byte 或 word order。signed zero 只有 `+0` 这一种 wire
spelling。decode 拒绝 negative-zero 与 non-finite spelling；若 host 不具备精确支持的 IEC 559
profile（包括 subnormal），codec 会在 compile time fail closed。

payload byte 留在 JSON 和 control frame 之外。IPC OutputStore 私下 stage 所有 buffer，并最后发布
完整 metadata manifest。worker protocol v3 传输 metadata 和 data-plane reference，而不在 control
frame 内放置 bulk byte。durable manifest 把相同 record 绑定到稳定 artifact/commit identity，同时
保留 manifest-last、barrier、replay、deletion 和 fail-stop 顺序。durable restart 会在分配前应用较小
且固定的 manifest 与 Job-record 上限；随后再依据 frozen archive 上限、剩余 tenant-retention quota、
manifest 声明的精确长度、物理长度与 non-sparse storage 检查每个 archive，之后才会分配或读取 payload。

每次 decode 都是 transactional。只有 framing、version、bound、canonical ordering、descriptor/
Layout digest、owner join、精确 payload length、SHA-256 以及本地内置或 provider validation 均通过，
才会发布任何内容。reconstruction 会创建全新的 allocation、Value revision、producer、fence 和
local binding identity。aligned CPU deleter 会保留匹配的 delete alignment；provider Value 与 indexed
lease 则保留完成验证的精确 generation/module 生命周期。包括后续 buffer allocation 中
`std::bad_alloc` 在内的任何失败，都会展开并释放此前所有本地 owner，不留下 partial result、formal
cache mutation、receipt、quota credit 或 dependent release。

## Cache 与持久化

memory cache 保留精确 immutable Value 与 validity fact。符合条件的 disk-cache/save 操作直接捕获
portable Value artifact，不会派生第二种图像表示。不受支持的 packed、quantized、device-only、
provider-missing 或 non-Ready input 会按显式 codec/artifact 契约 fail closed。

artifact identity、logical content digest、Value revision、allocation identity、graph revision 和
statistics-cache identity 相互独立。replay 可以保留同一 artifact/content identity，但必然创建全新
runtime identity。

## OpenCV 与普通 OpenEXR adapter

public OpenCV adapter 只接受 Ready、Host-readable、whole-byte、unquantized、Strided 的普通 image
Value。whole-Value view 会在任何 narrowing、address lookup 或 matrix-header construction 前，拒绝
超出 OpenCV `int` 范围的 width/height。`InputTile` view 则验证 source Value、ROI containment 与
ROI 自身的可表示 extent，然后直接使用 ROI size、原 row stride 和精确的 first-channel address
构造局部 zero-copy matrix，不会先构造 full matrix。因此，可表示的小 tile 可以查看 oversized
zero-stride logical image，同时保留精确 `step` 与 address。padded stride、zero-based OpenCV
metadata 与 signed Value origin 仍是彼此独立的关注点。borrowed read-only matrix 会保留 source
Value；mutable matrix 被限制在独占 Host output grant 内。OpenCV decode 保留受支持的 8/16-bit
code value，并分配显式 zero-origin data window，因为普通 OpenCV metadata 不具备 signed-window
authority。它从不处理 `.exr` path。
encode 使用封闭的 extension/depth/channel matrix：JPEG 只允许一或三 channel 的 unsigned 8-bit；
PNG/TIFF/JPEG 2000 接受声明的一/三/四 channel unsigned 8/16-bit 组合；BMP/WebP/Netpbm 保留各自
更窄的声明子集。signed 与 floating matrix 会在 `cv::imwrite` 前被拒绝，因此 OpenCV 无法静默
fallback 到 CV_8U。OpenCV 路径仍从不处理 `.exr`。

可选的普通 OpenEXR codec 只接受单 part scanline image。它独立保留 signed data/display window。
uniform UINT/FLOAT channel 保留 32-bit storage；HALF sample 会精确提升到 FP32，因为内置 tensor
契约没有 binary16 storage encoding。mixed channel storage、sampled channel、tiled/deep/multipart
input、缺少显式 encode display window 以及隐式 numeric conversion 都会被拒绝。

OpenEXR Deep 不使用该普通路径。它仍是一个 provider-defined multi-buffer `VariableSampleField`
Value，组合 ImageFacet、DeepSampleFacet、count、prefix offset 和每个显式映射 channel 的 sample
stream。零个或可变数量的 sample 永远不会被 padding 成 DenseTensor。

## 显式 sample conversion

storage 从不隐含 sample 含义。codec 和 CLI conversion 使用一个 `SampleConversion`，其中包含显式
source/destination `SampleEncoding`、有限 inclusive `SampleDomain`、destination semantics/storage，
以及 out-of-domain input、clamp、integer rounding、NaN/Inf 和 precision loss 的封闭策略。

identity conversion 不执行 scaling。semantic conversion 仅在完成 source-domain reject/clamp 后应用
已声明 affine mapping，再应用所选 rounding、representability、non-finite 和 precision 规则。精确
endpoint 与 equal domain 会绕过 arithmetic。finite direct span 会先计算 quotient；quotient finite 且
非零时，一个 endpoint-relative source distance 会直接与更接近零的 destination endpoint 做 fused
运算，symmetric destination 则在可用时采用稳定 source midpoint。若任一 direct span overflow，
source 与 destination endpoint 会分别做二次幂 normalization；有界 span quotient 加精确 exponent
difference 会在不构造任一原始 span 的情况下推导 candidate scale。normal derived scale 会直接乘以
原始、未缩放的 finite endpoint/midpoint distance；derived scale 为 zero、subnormal 或 infinity 时，
回退到 endpoint-relative fraction 加 fused destination interpolation。因此只有 interval endpoint 会
在 normal-derived-scale path 中做 normalization；可表示的小 input displacement 会保持未缩放，
直到进入 fused map。forward map 与 precision-reverse map 在
不要求 `long double` 比 binary64 更宽的情况下，仍能保留同号、跨零、窄 subnormal、ratio-underflow
与 cross-zero overflow-span case，且不会产生可避免的 infinity、NaN、zero radius、
rounded-midpoint ratio 或 premature zero。precision Reject 仍会用 exact destination storage 和 exact
reverse mapping 比较 working affine result；它不会预先舍入更宽的 `1/3` 来把 FP64 narrowing
伪装成精确。
作为 extreme midpoint 构造的 binary64 spelling 可能已在 working-type calculation 前完成舍入。
因此，其 portable oracle 会用 Allow 断言 nearest destination storage；Reject 成功向量只使用
endpoint 与 `0.5` 等可证明精确且可逆的位置，不假定 `long double` 是否比 binary64 更宽。
不存在隐藏的 255/65535 算术、color transform、channel-role inference 或 missing-metadata fallback。
equal endpoint/storage identity 通过 type-aware 比较读取 integer domain，并在不做 floating promotion
的情况下复制每个 in-domain native sample，从而保留 `int64_t`/`uint64_t` 在 `2^53` 附近及其极值的
精确值。若平台 `long double` 无法证明 source promotion 精确，non-identity wide-integer conversion
会在 affine arithmetic 前被拒绝；最终 floating-to-integer cast 还使用开区间上界，确保被向上舍入的
`INT64_MAX`/`UINT64_MAX` endpoint 绝不会授权越界 cast。

## 实现与验证映射

主要契约和实现：

- `include/photospider/data/{value,image_metadata,image_view}.hpp`
- `include/photospider/data/{sample_conversion,value_artifact}.hpp`
- `include/photospider/host/{host,value_result,value_artifact_result}.hpp`
- `src/lib/core/{value,sample_conversion,value_artifact}.cpp`
- `src/lib/adapters/opencv/{value_adapter_opencv,image_artifact_codec_opencv}.*`
- `src/lib/adapters/openexr/openexr_dense_image_codec.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/ipc/`、`src/lib/server/worker/` 与 `src/lib/server/state/`

长期测试覆盖 Value construction、signed coordinate、sample conversion、artifact reconstruction、
Host result、IPC lease、worker/durable replay、OpenCV lifetime、普通 OpenEXR round trip 和
provider-defined Deep 行为。source-residue search 仅是 migration evidence，不注册为 CTest 或 CI
behavior test。
later-buffer artifact 回归使用仅在 BUILD_TESTING 中编译的 source-private runtime failpoint，并在
选定的 `BufferHandle::ControlBlock` allocation 之前立即触发。production build 不编译 test-access
seam，测试也不替换 process 或 shared-library 的 global allocation 符号。
cross-zero overflow-span 回归同样使用一个仅 BUILD_TESTING、source-private、thread-local 的
scope，在仍调用 public `convert_dense_image_samples` 的情况下选择 binary64 affine working
arithmetic。嵌套 scope 会恢复此前 mode，并发线程彼此独立，header 不安装，production build
既不编译 selector，也不编译对应 branch。
