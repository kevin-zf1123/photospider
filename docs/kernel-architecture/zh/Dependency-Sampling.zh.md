# 依赖采样算子

默认 registry 通过分阶段 Result program 执行 `image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter`。STMap 接收 canonical image Result 与 generic 坐标映射 Result；radius gather/scatter 接收 rank-one Float64 数据及 Int64 radius Result，并发布 generic Float64 tensor Result。

STMap program 为每个输出像素记录精确的 Data 和 Control support，并单独记录 Validation 闭包。图像输出会将所请求的通道 footprint 闭包为完整 RGBA 像素，因此 Result coverage 和 dirty mapping 都按像素观察。Radius continuation 为每个请求的输出样本记录精确 Data 或 Control support，并独立记录 `close_samples` Validation。

## 范围与执行路径

`OperationRegistry` 将 `image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter` 注册为 Result operations。每个定义提供 `start_result` 来创建 `ResultContinuation`，并提供 `specialize_metadata` 来检查绑定的 Result schemas 与 parameters、推导执行前的 output schema。Continuation poll 接收 `ResultProgramPhase`；Coordinator 履行 Result object、tensor 和 I/O Needs，callback 只能读取这些 Need 授权的 tensor samples。`ResultProgramPhase::report_numeric` 用于报告有界 `NumericDiagnostics`。C plugin 接口单独由 Result operation ABI 2 定义，也使用 Result ports，不提供 Value dependency executor。

Registry 持有不可变 operation definition。Continuation 保留 prepared metadata 和 resource owners；每次 poll 借用当前 phase 与获准的 input capabilities。Callback 将实际消费的 source ObjectIds 记录在 output association 中，并发布 Result。

## STMap Result 契约

`image.stmap(source, map, boundary)` 要求 source Result 使用 `photospider.image` v1 schema，含一个 `pixels` tensor 且没有 fields，语义为 canonical 线性预乘 Float32 RGBA，sample shape 为 `[F,L,Hs,Ws,4]`，`batch_axes=[F,L]`。Map 是一个恰含一个 tensor 且无 fields 的 Result，可使用任意结构有效的 schema id/version/member key，dtype 为 Float64，shape 为 `[Ho,Wo,2]`，facets 为空。其 `batch_axes` 可以为空，此时同一 map 广播到所有 source frame/layer；也可以等于 source batch axes。Source H/W 轴最大为 2^40。输出端口 `value` 是 canonical `photospider.image` v1/member `pixels` Result，Float32 shape `[F,L,Ho,Wo,4]`，并保留 source frame/layer extents。

必需 String 参数 `boundary` 没有默认值，Empty 请求也必须提供：

| 值 | 长度为 `N` 的轴上整数 tap `i` 的地址规则 |
| --- | --- |
| `constant` | 外部 tap 使用透明黑，不读取 source pixel。 |
| `clamp` | 限制到 `[0,N-1]`。 |
| `wrap` | 模 `N` 的非负余数。 |
| `reflect` | 周期 `2N`，重复端点：`p<N ? p : 2N-1-p`。 |
| `mirror` | 周期 `2N-2`，不重复端点：`p<N ? p : 2N-2-p`。 |

`N=1` 时，所有非 constant 模式都选坐标零。这些映射使用全局 source 坐标，不以 fragment 为单位分别应用边界。静态准备会检查 canonical source metadata、map tensor shape/facets/batch 兼容性、source 轴上限和 boundary 名称。Empty demand 执行这些检查，但不请求 sample payload。

对 map 坐标 `u` 和 `v`，操作从 `l=floor(u-0.5)`、`t=floor(v-0.5)` 确定四个邻居。小数偏移、权重和通道输出为：

$$
f_x = u-\tfrac12-l, \qquad f_y = v-\tfrac12-t, \qquad w_{dy,dx} = (dy ? f_y : 1-f_y)(dx ? f_x : 1-f_x),
$$
$$
y_c = \mathrm{Float32}\!\left(\sum_{dy=0}^{1}\sum_{dx=0}^{1} w_{dy,dx} \, s_{dy,dx,c}\right).
$$

对于每个被请求的输出像素，同一个 continuation 首先以 Control、Validation 和 Descriptor roles（14）请求 map pixel，同时请求 source descriptor。它将两个 map 坐标校验为有限且位于 `[-2^40,2^40]`，计算四个经 boundary 映射的 tap，再以 Data、Validation 和 Descriptor roles（13）请求存在的完整 RGBA source tap。如果 `constant` 使所有 tap 都在 source 外，relation 仍包含 source descriptor，但不会请求 source payload。Map Control 与 source Data relation 仅覆盖选中的 map pixel 和 taps；typed Validation 则单独记录在 `close_samples` 闭包上。

Tap 顺序固定为左上、右上、左下、右下。每个位于 source 内的 tap 都会读取完整 RGBA pixel，包括权重为零的 tap；编辑该 source tap 时，即使权重为零且当前输出位不变，输出仍会被标记为 potential dirty。所选 map pixel 的 Control witness 独立追踪 map 编辑。地址关系将输出 pixel 紧凑映射到 source pixel，因此稀疏请求不会展平或枚举巨大 source image。四个乘积按 tap 顺序依次做 Float64 加法，最后一次转换为 Float32。Result writer 将请求的通道样本闭包到完整 RGBA pixel，保留全局 frame/layer 和空间坐标，并通过 all-or-nothing Result transaction 发布。

Program 的状态、tensor windows、workspace、输出和 work 都计入 Root limits。非法 map 坐标返回 `OperationFailed`；不符合要求的 image/map metadata 返回 `TypeMismatch`；未知 boundary 返回 `InvalidArgument`。Typed image source validation 可返回带 `FailureReason::InvalidDomain` 和 source `input_id` 的 `InvalidArgument`。取消和资源耗尽保留各自状态码。执行失败时不会发布部分输出。

当完整 spatial sample 的字节几何不可表示时，Result writer 会为请求区域使用 affine backing。因此，对 `Ho=Wo=2^40` 的 map 请求一个 sparse pixel 时，只需在 Root limits 内写四个 output samples，无需分配完整逻辑输出域；全域写入仍须满足 Root budget。下文的巨大 source case 也通过相同的稀疏映射避免枚举完整 source。

`test_dependency_sampling` 覆盖五种 boundary、map frame/layer broadcast、三像素 identity map，以及从由 16 payload bytes 支持的 2^40 x 2^40 broadcast source 稀疏读取。它还检查零权重 tap 仍保留 dirty evidence、constant boundary tap 不请求 source payload、Empty 跳过非有限 map，以及 10x10 fixture 在执行前后测量到的 live payload 增量小于 1 MiB。该 fixture 增量不是 context payload 限额、峰值内存或 RSS 声明。Source 设置 `atomic_trailing_axes=2` 时，Validation 会闭包到完整 source row（fixture 中 `H=1,W=3`），Data 仍只覆盖选中 taps。零权重 typed tap 的 RGB 非有限分量会由 source validation 返回 `InvalidArgument`；非有限 map 坐标则先由算子返回 `OperationFailed`。四种 caller rounding mode 得到相同 tie 结果，并恢复调用方模式。测试还检查 work 准入后的 active cancellation 将 payload 释放回 baseline；context 退休后仍可从保留 Result 读取值 7，并确认 association 含两个 source。

运行仓库内 focused test：

```sh
cmake --build build/kernel-dev --target test_dependency_sampling -j8
ctest --test-dir build/kernel-dev -R '^test_dependency_sampling$' --output-on-failure
```

Installed consumers 使用公开 Result workflow 检查 STMap 和 radius，包括 demand 替换与 generation 行为。复现安装包检查：

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_dependency_workflow photospider_sampling_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^(installed_dependency_sampling|installed_dependency_radius_workflow)$' --output-on-failure
```

## Result radius gather 与 scatter

`numeric.radius_gather(source, radius)` 和 `numeric.radius_scatter(source, radius)` 接收两个 Result。每个 Result 恰含一个 tensor 且没有 fields；两个 tensor 均为未分批 rank one，长度相同，`1 <= N <= 2^40`。Source dtype 为 Float64，radius dtype 为 Int64。输入 Result contract 接受的结构有效 schema id、version、member key 和 facets 均可保留。两个算子输出 `value` 端口为 `photospider.tensor` v1/member `samples` Result，Float64 shape `[N]`、facets 为空。

对输出索引 `o`，gather 使用 `radius[o]` 选择 source 索引；scatter 则对每个候选 `i` 使用 `radius[i]`：

$$
G(o) = \{i \in [0,N) : |i-o| \le r_o\}, \qquad S(o) = \{i \in [0,N) : |i-o| \le r_i\}.
$$

Gather 将 `G(o)` 化为裁剪后的半开区间。满足 `output < size` 且 `radius <= 2^40` 时，代码为：

```cpp
cursor = output > radius ? output - radius : 0;
end = output + 1 + std::min(radius, size - output - 1);
```

对每个请求的输出样本，Result program 以 Control、Validation、Descriptor（role 14）请求对应 radius 值，并同时请求另一输入的 Descriptor（role 8）。观测到 `[0,2^40]` 以外的 radius 返回 `OperationFailed`。Gather 随后以 Data、Validation、Descriptor（role 13）分块请求裁剪区间内的 source 值，每块最多 64 个样本，并按 source 索引升序累加。

Scatter 每块最多 64 个 radius 样本，扫描完整 radius tensor。它保留完整 Control witness，包括没有命中的候选，再仅对命中的 source 索引请求 Data。远端 radius 改变后，即使当前输出数值不变，也可能新增 source edge 并使输出 dirty；操作不需要逆向空间索引。

Dependency relation 将每个输出样本映射到精确的 source Data 或 radius Control support。`close_samples` Validation 单独以 role 4 记录。即使 Data 请求为空，两个输入的 Descriptor relation 仍存在，因此 typed validation 与数值支持相互独立。

每次 poll 向 Root 预留 32,768 字节 scratch 并计入 Run work。Continuation 使用 inline 数组保存至多 64 个候选索引，逐个处理请求 footprint 中的输出样本。每个和从正零开始，按 source 索引递增执行 Float64 左折叠；每次 poll 建立最近舍入和渐进下溢环境，然后恢复调用方浮点环境。非有限的命中 source、中间和溢出及非法的已观察 radius 返回 `OperationFailed`；typed Result validation 可使用 source `input_id` 和 `FailureReason::InvalidDomain` 返回 `InvalidArgument`。取消和资源耗尽保留各自状态码。

Empty demand 执行静态 metadata specialization，并返回不请求 sample payload 的空 Result。非空 demand 中，builder 逐点私有发布，全部请求样本完成后只 seal 一次。seal 前失败或取消不会返回部分 Result。输出 coverage 对应请求 footprint `Q`；只有调用者请求完整域时才生成完整输出。

## 执行状态与错误

```text
Empty 查询 -> 静态校验 -> 不调用 state 或读取 sample
非空 radius 查询 -> start -> Control Need -> 精确供给
  gather -> 每块至多 64 个 Data -> 有序折叠 -> 发布 sample
  scatter -> 分块扫描全部 Control -> Data 命中 -> 有序折叠 -> 发布 sample
```

Coordinator 在每轮 poll 提供由当前 Result Needs 授权的 object/tensor capabilities。Radius continuation 保存扫描游标、最多 64 个命中索引、阶段及 Float64 累加器。

## 校验、错误与行为证据

STMap 与 radius definitions 使用 `OperationDefinition::specialize_metadata` 执行静态 Result 校验。Registry 提供完整的 input Result metadata 和 parameters；specializer 校验声明的约束，并在执行前返回推导的 output schema。它不读取 tensor payload。`OperationRegistry::start_result` 随后校验已解析的 `ResultProgramQuery`，并调用 definition factory 创建 continuation。执行期间，`ResultContinuation::poll` 接收当前的 `ResultProgramPhase`，其中包含 callback 当前获准的 Result inputs 和 services；Coordinator 在各次 poll 之间履行 Needs。

`test_dependency_sampling` 检查有 seed 的 gather/scatter 结果、精确 Data/Control/Validation support、输出位不变但依赖关系变化的 radius edit、typed 与 opaque facets、命中非有限值、有序求和、Empty demand、取消和 Root 回滚。`N=2^40` 的 broadcast view 可用每个输入 8 字节的 backing gather 最后一个样本，无需扫描远端 Control 值。[G4 `--radius-only` workflow](../../../examples/g4_workflow/README.md) 检查 Result-based 动态编辑、demand 替换和保留输出行为。STMap 覆盖见上文。

## 算法观察组与数值诊断

C++ `OperationOutputTraits::atomic_trailing_axes` 将完整尾轴分为同一原子观察，适用于 CPU staged Atomic 输出；0 保持逐标量观察。通用 shape `{N,C}` 设置 1 时观察 shape 为 `{N}`；axis `{3}` 设置 1 时观察 shape 为 `{1}`。宿主将部分样本请求闭包为完整元组，统一计算、验证和出具证书，再返回请求样本的交集。Result executor 将 STMap 输出观察闭包到完整 RGBA pixel。编译器边 metadata、structured bridge 和 joint 兼容性检查均保留分组，operation identity 包含该字段。Result operation ABI 2 的 `ps_result_tensor_spec_v2` descriptor 也暴露 `atomic_trailing_axes` 字段，与 batch rank 和 layout 字段并列。

Structured Result callback 通过 `ResultProgramPhase::report_numeric` 提交有界 `NumericDiagnostics`。报告包含实际 CPU profile、实现身份、evaluated values、strict fallback 和有界原因分类。Result callback dispatch 前，host 会从 Root 资源中准入对应 output/backend 的 `OperationTiming` 记录。每个 poll 的报告只合并一次，包括返回 Need 或失败的 poll。Callback 失败时，host 会先锁存首个错误，再合并该次报告；报告合并失败不会替换原始错误。若 `BackendUnavailable` 符合 CPU fallback 条件，host 会在 CPU 重启前记录失败的 GPU attempt。

Facilities 示例还会让 singleton 上游 Result callback 在报告一个 evaluated value 后耗尽其配置的 Host 余量，并返回 `OperationFailed`、`ShortIo`、Io origin 和 Group scope。其 C2 caller 已报告两个 values，因此 joint grouping 启用或关闭时，聚合值都是 3，且 upstream failure identity 保持不变。Timing record 在 callback dispatch 前已准入，因此 callback 用尽剩余 Host capacity 后，记录这些计数也不必扩展 Root-owned container。

`NumericDiagnostics::evaluated_values` 统计已报告的算术尝试，包括之后失败的工作。`OperationTiming::computed_elements` 是另一个计数：对 Result publication，它统计新增的 field rows 和 tensor samples。后续 Result revision 只统计相较前一 revision 新增的 rows 与 coverage；cache-hit publication 不增加 computed elements。逻辑计数超出 `UINT64_MAX` 或 sample cardinality 不可表示时，计数会饱和到 `UINT64_MAX` 并设置 `computed_elements_saturated`；恰好等于 `UINT64_MAX` 的精确计数不会设置该 flag。诊断计数饱和不会使原本合法的 Result 失效。这些诊断不改变语义身份，也不能证明精度界。见手动[数值 workflow](../../../examples/numeric_workflow/README.md)。

Structured execution 会按 producer output 和 backend 合并每个实际执行 observation 的报告。Cache hit 和未请求的 observation 不贡献算术计数。

## 限制与调用方处理

`image.stmap` 使用 canonical `photospider.image` source Result 和 generic Float64 map Result；三个已注册 key 均使用 Result 输入与输出。`ValueFragments` 保留为 atlas preparation/materialization 使用的 typed-backing helper，不是独立的 workflow 或 dependency-execution 入口。对 pure 且 cacheable 的 Result program，completed Result cache 可在 frozen 或 generation identity 改变时复用 sealed output，条件是 tensor footprint `Q` 完全相同、typed transitive source proof 匹配当前 bindings，且 Need 重放后的 ready facts 不变。Cache hit 会创建新 ObjectId 和当前 association，同时共享缓存的物理 backing；同一 frozen execution 内的重复请求仍由 weak same-ID producer sharing 提供。Cache 要求精确匹配 `Q`；重建当前 dependency evidence 时，可能重新运行已被 eviction 的 Whole ancestor。Incomplete source owners、已观察到的 backend fallback 和 `photospider.path_set` 均不参与缓存；可选 cache work 或 Root capacity 不足会导致 lookup miss 或跳过 retention。缓存的 ownership 与 proof 契约见[结构化 Result 与 tensor slots](Global-Results.zh.md)。Radius scan 或输出准入超限返回 `ResourceExhausted`；typed source 验证遵循 Result 输入验证契约，非法数值观察返回 `OperationFailed`，取消由 execution host 传播。
