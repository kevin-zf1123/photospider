# 插件 ABI 与所有权

[英文权威文档](../Plugin-ABI.md)。

## Provider 选择与编译

`Photospider::data` 管理存储、Result 发布和资源计账。`Photospider::model`、`operation_runtime` 和 `program` 依次提供物理表、算子及绑定后的 Plan 服务。仅执行的宿主链接 executor SDK，通过冻结的运行时算子目录导入 v5 Plan。仅编译的宿主通过 compiler 组件绑定、规划并导出 Plan。`Photospider::kernel` 提供默认 compiler 和 physical executor。`Photospider::ops_runtime` 注册执行所需的内置算子。`Photospider::ops` 还提供 workflow 编写接口和默认编译期映射目录。

Planner 模块导出 `ps_planner_module_get_api_v1`。回调可读取 call 和 grid，按需请求单个 call 的静态 lowering，并追加任务或返回完整物理表。物理表传输使用 physical block 语法、调用绑定和宿主的构造预算。宿主在发布不可变 Plan 前核验任务图、输出覆盖、输入契约和资源。链接版和动态版 static-tiles 共用规划实现。

设置 `PHOTOSPIDER_BUILD_EXECUTOR=OFF` 可只构建编译端 CLI，设置 `PHOTOSPIDER_BUILD_COMPILER=OFF` 可只构建执行端 CLI。`Photospider::execution` 接受显式 provider 目录，`Photospider::execution_default` 增加链接版 physical 实现。

Executor 模块导出 `ps_executor_module_get_api_v2`。准备阶段核验 Plan 版本、backend、内存域和 invocation mask。Prepared owner 归属创建它的 context。提交快照在同步调用内借用。异步 provider 保留需要的快照服务，持有已接受任务至完成，并在关闭时排空执行和观察回调。宿主服务提供物理表、prepared kernel、Result 构造和存储保活。Provider 在相应模块租约有效期间释放 native allocation 和回调。发布中间 call 导出值时，provider 可通过修订号 `"1"` 查询纯 C 服务 `PS_EXECUTOR_VALUE_RESULTS_V1`。其 `make_result` 接收物理 value ID，按已绑定的 schema 和 context 资源预算创建 builder。最终输出也可以按名称创建。资源根持有执行器租约，宿主保留动态线程的租约直到 join 完成。Metal completion block 和页释放 block 由宿主持有。随包的 physical 模块先协商同构建的 SDK 扩展，再访问公共 Program、Result 和资源对象。SDK 构建身份不同会返回类型不匹配。独立 C executor 直接调用带版本的宿主服务。

### Executor 退休事件

`ps_executor_module_v2` 与 `ps_executor_submit_v2` 使用 executor ABI 2；布局未变的 host、Plan、result 和 owner 记录保持版本 1。Loader 要求 `ps_executor_module_get_api_v2`，不回退到旧入口。

`ps_executor_submit_v2::retire` 在 sequence、activation、source-call 标识之后接收 `call, outcome, failed_task, status`。`PS_EXECUTOR_TASK_NONE_V2` 为 `UINT32_MAX`，任务 0 是合法 ID。Failed 要求非 OK status、属于该 call 的任务和空 values。Succeeded、Elided 要求 OK 与 NONE，Elided 的 values 也必须为空。宿主对照保留的提交 Plan 校验 call/source/task 归属及 envelope，即使没有 observer 也执行校验。非法事件返回 `InvalidArgument`。成功事件必须提供该 call 声明的全部导出值，各 Result 的 schema 和覆盖区域须与物理计划一致。

宿主将失败任务写入 `CallRecord::failed_task`，并在 status detail 中设置 Backend/Run 及对应 source call/task ID。Status code、reason 与不透明 domain code 经过正常 status 转换保留。异步 provider 在使用期间持有提交服务，并在 join/close 返回前完成 observer 交付。观察回调抛出异常后，宿主停止向该 Run 交付后续记录，并在已接受工作排空后返回带节点和退休序号的错误。

### Operation 与 mapper 选择

Operation 模块通过 `ps_operation_module_get_api_v10` 导出算子、mapper 或两者。重名或并发冻结导致整批注册失败。算子提供完整 lowerer 或逐 tile 任务描述。Direct 将输出逻辑区域分别投影到实际输入 grid。Local 和 External 以有序 source slot 返回 tile 区间或仅 schema 的依赖。外部依赖声明 alias、contract、provider、契约版本和实现修订号。规划会话按需解析依赖并对重复求解计费。v5 Plan 保存最终读写、placement、参数和 scratch。导入、裁剪和 kernel preparation 均直接使用这些记录。

## 范围与职责

受信任的 native module 导出 operation module ABI 10。Loader 将静态声明复制到 `OperationRegistry`；C module 与 C++ `OperationDefinition` 回调使用相同的 `OperationProgram` builder、物理 Plan validator 和固定 invocation ABI。OperationTraits 使用版本 30。

Definition、静态 preparation 或 prepared kernel 需要模块代码时，registry 及其派生 owner 保留模块。Compiler 拥有静态 metadata 与物理任务图。`PreparedPlan` 拥有已链接 kernel 和资源；每个 Run 拥有 bindings、输出分配、scratch 和完成状态。CPU kernel 接收固定仿射 view，并按 [unchecked 执行契约](Compiler-and-Execution.zh.md)写入预分配输出。

## Module 与声明记录

```c
#include "photospider/plugin/operation_module_api.h"

/* 模块提供此函数及不可变表。 */
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v10*
ps_operation_module_get_api_v10(void);
```

`operation_module_api.h` 是独立的 C11 头文件，通过 `fixed_invocation_api.h` 引入 status、layout、invocation 与 prepared-kernel 记录。构建生产者时定义 `PHOTOSPIDER_OPERATION_PLUGIN_BUILD`。`Photospider::operation_sdk` 提供 include 目录，C 生产者无需链接内核。

Module 表包含精确的 `struct_size`、`abi_version = 10`、0..1024 条 operation 和 0..1024 条 mapper 记录（两者合计至少一条）、context 和必需的 `destroy` 回调。每个 operation 声明 key、能力标志、输入契约、输出原型、标量参数词汇、可选静态 preparation 与资源校验、完整 lowerer 或 tilewise 声明，以及可选 kernel preparation。仅包含 COPY 或完全 elided 的 operation 可以省略 kernel preparation。

| 记录 | 静态含义 |
| --- | --- |
| `ps_schema_v8` | Schema identity/version、1..16 个 tensor member，以及可选 schema publication metadata |
| `ps_tensor_spec_v8` | 元素类型、cell shape、batch 轴、atomic trailing axes 和空间布局；完整 rank 至多为八 |
| `ps_port_contract_v8` | Schema identity 和可选 member dtype/rank 谓词 |
| `ps_parameter_spec_v8` | 精确 Int64、Float64、Bool 或 String 类型、required 标志与可选数值边界 |
| `ps_operation_output_v8` | 输出名称、完整原型和可选原始输入投影 |
| `ps_resource_id_v8` | 不可变资源的字节长度与 SHA-256 |

C 字符串携带显式字节长度，空字符串使用 `{NULL, 0}`。Key 使用严格 UTF-8，参数字符串值可以是任意字节。Operation key 最多 1024 bytes，其他声明 key、kernel symbol、contract 与 placement 字符串最多 128 bytes。数组在且仅在计数非零时使用自然对齐的非空指针。带 `struct_size` 的记录要求大小精确匹配。

兼容策略保留 exact-size 校验。每个 ABI 版本固定记录布局、函数签名、数值编码和字段语义，包括所有权及线程规则。改变这些契约时，必须升级受影响的 ABI 版本。为已有数值增加名称不改变二进制契约。Host 接受本文列出的版本；结构不进行前缀或尾部字段协商。

独立 C 头文件定义完整的数值词汇。枚举常量为固定宽度整数字段中的值命名，ABI 记录不内嵌 C enum 对象。`ps_element_type_v8` 定义以下编码：

| 常量 | 编码 | `element_type_mask` 位 |
| --- | --- | --- |
| `PS_ELEMENT_UINT8_V8` | 1 | `PS_ELEMENT_MASK_UINT8_V8 = 0x01` |
| `PS_ELEMENT_INT64_V8` | 2 | `PS_ELEMENT_MASK_INT64_V8 = 0x02` |
| `PS_ELEMENT_FLOAT64_V8` | 3 | `PS_ELEMENT_MASK_FLOAT64_V8 = 0x04` |
| `PS_ELEMENT_FLOAT32_V8` | 4 | `PS_ELEMENT_MASK_FLOAT32_V8 = 0x08` |
| `PS_ELEMENT_INT8_V8` | 5 | `PS_ELEMENT_MASK_INT8_V8 = 0x10` |
| `PS_ELEMENT_UINT16_V8` | 6 | `PS_ELEMENT_MASK_UINT16_V8 = 0x20` |
| `PS_ELEMENT_INT16_V8` | 7 | `PS_ELEMENT_MASK_INT16_V8 = 0x40` |

端口可选择一个非零元素编码，或对 `PS_ELEMENT_MASK_*_V8` 常量按位 OR。两个字段均为零时不增加 dtype 限制；同时非零属于非法声明。每个位为 `1u << (code - 1)`；`PS_ELEMENT_MASK_ALL_V8` 为 `0x7f`，其他位会被拒绝。零仅用于端口通配，不是 Tensor 元素类型。

Schema scope 使用 `PS_SCHEMA_COMPLETE_BUNDLE_V8 = 1`、`PS_SCHEMA_INDEPENDENT_CHUNKS_V8 = 2` 或 `PS_SCHEMA_STABLE_PREFIX_V8 = 3`。这些值描述 schema publication metadata；固定执行仍以完整 task 为完成单位，并在 call 退休或 RET 时发布 Result。重复输入使用 `PS_REPEATED_MATCH_DTYPE_SHAPE_V8 = 1`，要求选定 Tensor member 的 dtype 和包含 batch 轴的完整逻辑 shape 相同。`PS_REPEATED_MATCH_NONE_V8 = 0` 允许差异；启用重复输入组时，该模式要求静态 prepare 回调。停用的组使用空模板指针和零 bounds/match code。布尔字段使用 0 和 1。

一个 operation 最多有 1024 个输入、64 个输出与 128 个参数声明。重复输入在固定输入前缀后增加一个同构契约，并显式声明最小/最大数量。`input_count = UINT32_MAX` 配合空投影指针选择全部原始输入；零表示不读取 payload 输入。静态 preparation 可以收窄注册投影。Schema-only 依赖在 lowering 时单独声明。

`load_plugin(path)` 接受 1..4096 bytes 且不含内嵌 NUL 的显式路径。缺少 ABI 10 入口时返回 `TypeMismatch`。空、未对齐、大小错误或版本错误的 module 表返回 `InvalidArgument`。Loader 检查 module header 后才挂接 destroy hook 并读取嵌套记录。它在私有候选 registry 中验证全部声明，然后原子发布所有 operation。声明非法、key 重复或并发 freeze 均不改变当前 registry。

## 静态 preparation 与资源

对 C++ operation，`OperationTraits` 声明输入数量与端口约束、重复输入规则、命名输出原型与投影、参数类型与范围、纯度、支持的 backend 和 extension identity。`OperationPreparation` 返回输出特化、所需资源 identity 与不可变 state。这些静态契约进入 compiler identity；回调地址与可变 invocation state 不进入 identity。

Lowering 按内存域声明每个 task 的 scratch，并声明每个输出 tile 的分配契约。共享中间计算使用私有 SSA value 与 task edge。`side_effect_free = false` 的 operation 必须恰好生成一个 terminal effect task；即使输出未被选择，planner 仍保留该 call。Block admission 在 kernel 执行前依据物理表计算输出容量与每个 lane 的 scratch。

`ps_operation_v10::prepare` 接收 `ps_static_query_v8`、同步 preparation 服务和初始为空的 `ps_operation_state_v8` 输出。Host 在静态回调前复制输入 schema，并保留参数值及其浮点位模式。回调可以为每个输出设置一次 schema，省略的输出保留原型；通过 `require_resource` 声明资源 identity。

返回的 state 不可变，在输出选择、lowering、kernel preparation 和并发 Run 之间共享。非空 context 必须提供 release 回调。空 context 也可以带 release 回调，其清理 owner 仍被保留。无论成功或失败，已填充 state 的所有权都转交 host，并由 host 恰好释放一次。需要保留的回调输入必须复制到算子拥有的存储。

Schema 特化后，host 从提供的 `ResourceSet` 解析声明的资源 identity。缺少资源会拒绝 preparation。可选 `validate_resources` 回调只检查这些不可变字节和静态 state，每次 preparation 调用一次，覆盖 Empty 输出需求和 artifact 重新绑定。Kernel preparation 借用相同资源的 view，资源 owner 持续保留。Lowering 不获得资源字节或 sample payload。

Preparation 服务在返回前复制提交的 metadata。服务仅接受回调线程的调用，并保留首次失败。被忽略的服务失败优先于后续回调 status 或异常。Preparation sink 最多接受 1,048,576 次调用和 4096 条资源声明；schema 与输出边界独立适用。

## Lowering 为物理 Plan

`ps_operation_v10::lower` 接收已解析的输入/输出 schema、参数、静态 state、tile extent、backend、member grid、选定输出 tile 区间和构造限制。每个 grid 标识原始 port/member，并给出完整 shape、tile 数量、行列数和 batch-axis mask。请求区间是 member 局部 tile 索引的半开区间。

`ps_operation_builder_v10` 提供经过检查的构造方法：

- `add_task` 声明 kernel symbol/contract、placement、各内存域 scratch、可选终端 effect，以及显式 invocation ABI（8 或 9）。ABI 9 要求 CPU placement；非法选择锁存 builder 失败。
- `add_copy` 声明显式传输，要求一项相匹配的读取与写入。其 backend 独立于数值 task 选择：native GPU Plan 可包含用于 Host/Shared 传输的 CPU COPY task。CpuExact 请求要求 CPU task。GPU lowering 可在 operation 声明的能力范围内显式混合 CPU/GPU 数值 task；包含 Device 端点的 COPY 要求 GPU placement。Builder 为 GPU COPY 的每个 Host 端点声明一段 dense Shared scratch，大小等于输出字节数。该声明参与序列化 Plan 校验和每个 lane 的准入。
- `add_temporary`、`write_temporary` 和 `read_tile` 连接 call 内的私有 SSA tile。
- `read_input`、`read_input_schema` 和 `write_output` 声明 payload 读取、metadata 依赖和独占输出 tile。
- `depends_on` 添加顺序边，`set_parameters` 复制不透明的 task 参数字节。

ID 属于当前 call 的 builder。每个物化输出是完整固定网格 tile，且只有一个 producer。Host 在发布 Plan 前校验输出覆盖、拓扑、call 归属、placement 和分配边界。Builder 失败保持锁存。服务调用数与累计参数字节分别受 `maximum_work` 约束；表记录与引用使用 builder 的 record/edge 限制。回调成功不能覆盖已失败的 builder 操作。

私有 task 提供共享中间计算。Tile 边决定就绪条件，call 顺序决定退休顺序。Schema-only read 不产生 payload 依赖。序列化 Plan 的导入恢复已保存的表并重新绑定静态 preparation，不调用 lowering。

`ps_tilewise_v10::configure` 与 `describe` 均在 `static_state` 后接收选定的 `PS_BACKEND_CPU_V8` 或 `PS_BACKEND_GPU_V8`。Configure 在每次 lowering 中调用一次，向 mapping 提供至多 64 个配置 word；describe 为每个请求 tile 选择 symbol、placement、invocation ABI、参数和 scratch。Query 仅在 callback 期间借用，不同 lowering 可以并发调用。布局未变的 lowering query、tile task、mapper 和 fixed invocation 记录保持各自独立版本。

## Kernel preparation 与调用

```c
/* ps_operation_v10::prepare_kernel 的签名。 */
ps_kernel_status_v8 prepare_kernel(
    void* user_data, const ps_kernel_preparation_v8* query,
    ps_prepared_kernel_v8* result);
```

Query 包含 kernel symbol/contract、精确 placement、完整静态 metadata、实际读写 view、task 参数字节、scratch 边界、资源和 native-service 指针。每个 view 包含原始 port/member、tile index、domain、编译所得 tile 的字节范围、字节偏移和有符号仿射 layout。私有写入使用 `port = UINT32_MAX`。Device 输入可以绑定较大 allocation 内的子视图，此时运行期容量和偏移包含经过校验的 allocation base。声明的 layout 仍限定允许访问的样本。

C++ `KernelPreparation` query 从不可变 owner 借用 operation name、输入 metadata、scalar parameter map 与编码后的 task parameter vector。其输出 metadata 和 read/write view 数组属于临时 preparation 存储。这些数据均在 callback 期间有效；复制 query 保留借用引用，不延长引用目标的生命周期。Provider 要在返回后保留数据时，必须复制所需的各项值。C bridge 数组保持原有 callback 期间有效的生命周期。

Module 在此检查调用签名，并返回带直接 CPU 和/或 GPU 入口的不可变 prepared context。非空 kernel context 必须提供 release。即使 preparation 报告失败，已填充 kernel 输出的所有权也转交 host；准备失败和返回 ABI 记录非法时都会释放该 context。成功的 kernel owner 保留静态 preparation、资源 backing 和定义它的模块。

CPU invocation 借用输入、独占输出、scratch、task 参数和取消查询，直至同步返回。成功意味着所有声明输出样本已经初始化，且全部访问已经结束。Host 恢复回调的浮点环境。Kernel 可重入；可变状态属于当前 invocation 的 scratch 或输出分配。

`ps_kernel_status_v8` 携带操作性错误码、reason、可选 domain code 和有界 NUL 结尾消息。Status 数值由 `fixed_invocation_api.h` 定义：`ps_kernel_code_v8` 定义类别，例如 `PS_KERNEL_NOT_FOUND_V8 = 2`、`PS_KERNEL_CANCELLED_V8 = 6`；`ps_kernel_reason_v8` 定义从 `PS_KERNEL_REASON_NONE_V8 = 0` 到 `PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8 = 13` 的 reason。Host 将 `domain_code` 作为不透明的算子自有值保留。未知 code/reason 或未终止消息转换为 `Internal`。运行期 sample 校验不属于此协议。

Executor 也可链接使用 Host、Shared 或 Device 存储的异步 GPU 入口，并在提交前设置完成槽。接受提交后，invocation view 和 scratch 持续借用直至匹配的终态 callback；拒绝提交不转移借用。完成可以在提交返回前发生。整个 context 的 GPU 额度只在完成或本地拒绝结算后归还。Native Shared buffer 在 block 发射前分配，可由 CPU kernel 或显式 COPY task 访问。Native preparation 可链接使用 Shared 与 Device 存储的 Metal MSL 或 Vulkan SPIR-V kernel。Device 分配提供 opaque identity 并保持未映射，已完成输出通过 Result publication 继续持有 owner。Executor 在内部准备 COPY task，无需调用算子的 kernel callback。

`ps_kernel_completion_v8::report_timing` 是可选的计时请求与接收入口。接收入口非空时，provider 可在 `finish` 前串行调用一次，传入借用的固定大小 `ps_kernel_device_timing_v8`；拒绝提交不得报告计时。Host 将记录复制到已设置的槽，不分配内存。报告不会结算 task 或释放借用；非法 key 或重复报告不能结算其他 submission。不可用的计时可省略或使用零 clock domain；计时查询失败不改变 task 成功状态。Metal 使用 system mach 纳秒。Vulkan 报告 raw tick、有效位数、每 tick 纳秒数与进程内 clock instance。单个 pair 的差值按有效位数作模减；Vulkan 跨提交比较需要本接口之外的校准。两种时钟均不等于 Run 的 host steady clock。Native 实现在 quiescence 后报告 command 区间，原有完成与可见性契约继续生效。

### Typed CPU invocation 9

可信 C module 通过 `ps_operation_v10::prepare_kernel_v9`、C++ registration 通过 `OperationDefinition::prepare_kernel_v9` 为 `abi_version=9` 的 CPU `KernelDesc` 提供工厂。`fixed_invocation_v9_api.h` 中独立声明 `ps_prepared_kernel_v9` 与 `ps_kernel_invocation_v9`，各自要求精确 `sizeof(record)` 和版本 9；外层 envelope 包含保持精确 size/version 8 的普通 `views` 记录及 `ps_kernel_work_v9` 服务。v8 记录的大小和语义保持不变，不作 prefix 协商。Preparation 与 invocation 使用对应的 typed 入口；没有匹配 factory 时拒绝，不降级 ABI。用 v8 invoke 重载调用 v9-only preparation 会在 provider 入口前拒绝。

线程安全的 work 服务在数值计算前，对本次 Run 与共享 Context root 准入 `{work, stages}`。零/零调用查询取消或 sticky Run 预算失败。预算拒绝返回 `ResourceExhausted` 及 `WorkLimit`/`StageLimit`，取消可返回 `Cancelled`。服务 context 仅借用至同步 CPU 返回，属于本 invocation/Run，不得存入 prepared state 或传给其他 Run。Host 持有全部 owner 至 CPU 返回。Provider 算术建立所需舍入和 gradual-underflow 环境，host fence 保存并恢复 callback 环境；容量异常保留资源失败分类。

FRQ-01..06 使用 CPU invocation 9 的独立工厂。Operation module 分别导出 invocation 8 和 9 的 kernel preparation 回调。

## Native kernel preparation

包含 `photospider/plugin/native_kernel_api.h`，通过 kernel preparation query 中的 `ps_native_gpu_service_v2` 使用原生服务。调用 `prepare(context, program, query, result)` 前检查服务版本与 backend。服务表在外层 preparation callback 期间借用。调用成功时转交一个不可变 `ps_prepared_kernel_v8` owner；失败时，有效的输出记录保持为空。

`ps_native_program_v2` 描述一次 dispatch，包括 source、entry、bindings、constants、三维 grid 和可选 group 维度。Metal 接受 1..262144 bytes 的 MSL source 和 1..128 bytes 的 UTF-8 entry。最多使用 31 个不同的 buffer index；存在 constants 时，其独立 index 也计入此限制，constants 最多 4096 bytes。Grid 各维为正且不超过 `UINT32_MAX`。Group 各维为零时由设备选择大小；显式 group 要求三个维度均为正且满足设备与 pipeline 限制。显式 group 对发射范围向上取整，因此 shader 必须保护超出 grid 的 invocation。

Vulkan 接受四字节对齐的 SPIR-V 1.0..1.5 module，大小为 20..262144 bytes，且由完整 word 组成。选定的 compute entry 必须声明正整数固定 `LocalSize`。Group 各维为零时使用 module 声明的大小；显式维度必须与其相同。Preparation 检查设备的 group、grid、descriptor 和 buffer 范围限制。Constants 在 preparation 时复制到一个不可变 coherent uniform buffer；其实际 native capacity 在 prepared owner 的生命周期内持续计入设备 root。

每个 binding 从固定 invocation 中选择 input、output 或 scratch view。Input/output 的 `view` 是 invocation 数组索引；scratch 的 `view` 是内存域编码。两个 native adapter 均接受 Shared view 与 Device handle，并按 preparation 签名检查内存域和范围。Device byte offset 选择原生 buffer 内的位置，不对 opaque identity 进行指针运算。每个输出必须恰好绑定一次，且覆盖完整可访问范围。输入数据保持不可变。Shader 代码必须遵守这些声明并初始化全部输出样本。

Preparation 编译 pipeline，并复制 binding 记录、constants 和 dispatch 维度。Metal 使用 MSL 2.4，禁用 fast math，并在 source 前加入 `#pragma clang fp contract(off)`。返回的 kernel 独立持有 pipeline 与 device，其生命周期不依赖 pipeline cache 或调用方 descriptor 存储。提交时解析实际设备拥有的 buffer，并提交一个 command buffer。Metal 使用 command-buffer completion handler。Vulkan 为每次 submission 分配独立的 command、descriptor 和 fence 对象，由一个设备完成线程按队列顺序排空已接受的 node；它先释放 submission 资源，再交付终态完成。两条路径均在结束 invocation 借用前建立可见性。设备 command 失败会将设备标记为不可用并中止 Run；已接受的 submission 各自持有 owner，直至自身终态完成。

Native pipeline key 和 prepared context block 计入设备显式持有的 metadata root。调用方的线程局部 allocation scope 不改变该归属；unmanaged device 保持 unmanaged。Pipeline cache 构建和 context 分配可分别在 metadata 压力下释放 cached pipeline，并各自重试一次准入。Prepared kernel 独立持有 pipeline，因此释放 cache 可为其他 context 腾出容量，同时保持已接受工作的有效性。回收后仍耗尽时返回 `ResourceExhausted`，且不转交 kernel owner。

`test_fixed_native` 使用 C11 operation module 与真实 Metal shader，验证 prepared pipeline、有依赖及独立 tile task、Plan 导入、部分及空输出选择、并发输入绑定和显式 Host/Shared COPY 链。直接服务检查覆盖非法 descriptor、清除 cache 后继续存活的 prepared pipeline、Shared scratch、带 padding 的三维 group，以及完整 4096-byte constant payload 的复制。`test_fixed_async` 独立验证受控拒绝、非法 completion、取消排空和 backend failure。Backend failure 用例持续保留已接受工作直至完成，停止后续发射，并在存在 CPU 入口时仍保持 GPU placement。这些用例使用受控 completion，不注入原生设备故障。`test_native_gpu` 检查设备 root 计费、cache 回收、带 barrier 的完整 threadgroup、Metal contraction 策略，以及已接受工作被取消后的完成可见性。`test_fragment_atlas_gpu` 使用固定 native kernel 查询稀疏 atlas payload，并检查 rank 1、3、8 的 sample 位模式和空洞。`test_vulkan_gpu` 覆盖 SPIR-V 独立输入输出、三维 dispatch、并发复用 prepared kernel、constant 容量、metadata 回收与回滚、分配回滚、固定 Plan 执行，以及从完成 callback 析构 Device。

## 生命周期与失败边界

| Owner | 释放条件 |
| --- | --- |
| 回调输入数组和服务表 | 回调返回；保留这些借用地址无效 |
| 静态 operation state | 最后一个 preparation/kernel owner 释放，且 kernel context 清理已完成 |
| Prepared kernel context | 所有借用 invocation 完成后，最后一个 kernel owner 释放 |
| Module 表与代码 | 最后一个 definition/static/kernel lease 释放；module destroy 先于 native unload |
| 输入、输出、资源 backing | 最后一个 owning handle 释放，独立于 registry 生命周期 |

Host 对 preparation/lowering 异常设置边界，并将分配失败转换为 `ResourceExhausted`。C callback 和 release 函数必须遵守不抛异常的契约。被拒绝的 preparation 仍执行服务错误选择与清理。永不返回的 callback 可以阻止 drain；取消需要协作，不能授权提前释放。

Module 以宿主进程权限执行。ABI 不提供沙箱、签名准入、IPC 加载、运行期依赖发现、continuation polling、隐式传输、数值认证或 backend 重试。Backend 选择与 COPY 拓扑属于编译。[Compiler 与 executor 契约](Compiler-and-Execution.zh.md)定义有序 call 退休、最终 RET 发布和取消排空。
