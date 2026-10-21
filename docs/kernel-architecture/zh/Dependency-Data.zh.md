# 静态依赖与固定 Invocation

状态：Proposed breaking design。本页定义 [Plan 契约](Compiler-and-Execution.zh.md)中面向算子的部分。[英文权威文档](../Dependency-Data.md)。

## 静态 mapping 契约

Compiler 解析全部读取与 task 拓扑。Mapper 读取固定 schema、grid、静态参数、实现 profile 和选中输出，返回有限 input tile 集合及内部 stage 图。它必须确定、有界且不执行运行时 I/O。Payload 不成为隐式 mapper 输入。

静态 preparation 描述 output schema、input projection、内部 SSA tile/task、实现 symbol、参数编码、scratch 上界和 effect。Compiler 将其展平并校验。共享算法可在一个 task 中产生多个独立输出 tile。固定 invocation 路径不能在运行时增加依赖或 task。

执行 ABI 提供固定 invocation。以下 C++17 草图描述逻辑接口，不是最终 C ABI 8 的记录布局：

```cpp
#include <cstdint>
namespace ps_invocation_design {
struct Layout {
  std::uint32_t rank, element_type;
  std::uint64_t shape[8], origin[8];
  std::int64_t strides[8];             // bytes; resolved during prepare
};
struct ConstView {
  const void* allocation;             // opaque native handle in a device domain
  std::uint64_t bytes, byte_offset;
  const Layout* layout;
  std::uint32_t domain, member;
  std::uint64_t tile_index;
};
struct MutableView {
  void* allocation;
  std::uint64_t bytes, byte_offset;
  const Layout* layout;
  std::uint32_t domain, member;
  std::uint64_t tile_index;
};
struct Invocation {
  const void* prepared_kernel;        // immutable context with resource owners
  const ConstView* inputs;
  std::uint32_t input_count;
  const MutableView* outputs;
  std::uint32_t output_count;
  const MutableView* scratch;
  std::uint32_t scratch_count;
  const void* parameters;
  std::uint64_t parameter_bytes;
  const void* cancellation;           // opaque borrowed service
};
using CpuInvoke = std::int32_t (*)(const Invocation*) noexcept;
}  // namespace ps_invocation_design
```

Plan 固定 port/member/tile 顺序、view 和参数编码。Prepare 把 layout ID 解析为含 rank、shape、origin 和 byte stride 的不可变 descriptor，并绑定包含不可变资源和直接入口的 PreparedKernel context。Invocation 借用解析后的 descriptor 与 context；kernel 无需查 registry 或全局 layout-ID 表。

Layout 的 rank 为 1..8；shape 是 view extent，origin 是逻辑 tile 原点，strides 以字节表示，byte_offset 是 allocation 内局部坐标零点的位置，bytes 是 allocation 可访问容量。Prepare 校验有符号 stride 的全部可达地址。PreparedKernel 必须支持不同 Run 的并发只读 invocation。

Schema-only input 是不带 payload handle 的 prepared metadata。Invocation 数组保持到完成。Output 独占写入、input 不可变、scratch 私有。Adapter 在 C ABI 边界前捕获 C++ 异常。Status 报告操作性或算子主动报告的错误，executor 不验证数值结果。

## 依赖构造

Task `t` 的 `D(t)` 是 payload read 和显式内部 stage 边引用的不同 producer task 集合。传入不可变值在 activation 时已经可用，不增加局部 predecessor。建立 `pending[t] = |D(t)|` 和转置 successor 列表。同 producer 的重复读取只产生一条 readiness 边，不同 tile 仍需分别保留生命周期 hold。

[原型](../prototypes/ooo_executor.py) 接受 `Task.dependencies`，内容为从零开始的静态 task 索引。Predecessor 集合为这些显式边与 `reads` 推导的 producer 的并集，两者之间的重复边只计一次。只要 call 顺序与无环约束允许，显式边可以引用物理表中更后的条目。非法索引、自依赖、环和对更晚 call 的依赖均在准入前拒绝。

纯顺序边不增加 read view，也不增加 consumer hold。例如 effect 可以依赖一个不产生 payload 的 preparation task。若 preparation 有无人读取或导出的临时输出，其 buffer 可在完成时释放，早于 effect 启动。显式边仍要求 predecessor 成功完成，失败或跳过的 predecessor 不允许消费者发射。

例如 A 产生 `a0,a1`，B 通过两个端口读取 `a0`，通过一个端口读取 `a1`。B 有一个 predecessor、两个 hold 和三个 read-view 条目。A 完成一次，B 就绪一次。两个 allocation 都保持到 B 完成。Descriptor 是编译期事实，其读取不增加 producer 边。

Task 成功后全部 write set 可用。一个 tile 的消费者只等待其 producer task，不等待该 call 中所有 task。数值认证与 `CompleteBundle`/`StablePrefix` 校验闭包不建立依赖。真正的 reduction 和耦合算法显式声明其计算 stage。Call retirement 仍等待所有选中 call task。

数据相关读取声明静态超集。已证明 displacement bound 的 STMap 可以只读取有限邻域；任意 map 需要完整 source grid。Kernel 在提供的 view 内选择地址，并负责边界/NaN 语义。根据 payload 发现 producer 不属于该协议。

## Effect、控制与选择

普通 task 只修改声明的 output/scratch。外部 I/O 或共享可变状态需要 effectful call，由纯 preparation 和一个终端 `Effect` 组成，在退休头启动。注册实现契约声明 effect freedom，输入 schema 不能证明这一点。

Condition、edge argument 和跨 block 存活值是静态 root。Runtime 只激活选中 block，把参数映射到不可变传入值并创建新状态。[并行执行](Parallel-Execution-Model.zh.md)规定失败与 abort 顺序。

初始规划中缺失 output request 表示默认完整覆盖；`select_outputs` 中省略名称表示不导出。空请求不携带 payload，但 mandatory control/effect 保留。选择在已编译工作上求闭包；新覆盖需要编译。Executor 不生成 Need record、内容比较缓存或 dirty-propagation index。

## CPU/GPU 所有权与 ABI

CPU adapter 在一个 worker 上调用，并在返回后完成。Kernel 可轮询 cancellation，但必须在返回前结束全部内部工作。并行 stage 降低为 Plan task，基础版本不提供嵌套 range service。不可变 preparation 可共享，可变 invocation 状态属于单个 Run。

GPU adapter 使用固定 view/parameter 编码 dispatch/transfer，返回 accepted ticket 或拒绝。[提交规则](Parallel-Execution-Model.zh.md#cpu-与-gpu-完成边界)覆盖立即完成、恰好一次事件、可见性和 drain。Input、output、scratch、参数字节、module 和 command object 保持到真实完成。

已实现的 C module 与 fixed invocation ABI 8 使用固定宽度 count、精确 size/version record、不可变 input、独占 output/scratch、不透明 native handle、status 和 completion service。公共头文件为每种记录定义所有权及借用关系，不导出 STL layout。旧 module getter 被拒绝。Native GPU service ABI 2 独立定义异步 Metal/Vulkan preparation 与已接受工作完成前的所有权。具体记录和验证见 [Plugin ABI](Plugin-ABI.zh.md)。

## Checked 边界与 unchecked 执行

| 边界 | 所需检查 |
| --- | --- |
| 编译/import | 类型、grid、声明覆盖、边界、支配关系、DAG、归属、资源向量和 effect |
| Module link/prepare | ABI/version/size、symbol 契约、资源、kernel profile 与设备能力 |
| Run preflight | Name、schema、物理 coverage/layout、owner、当前性与资源准入 |
| Coordinator | 状态、completion identity、额度、取消与生命周期转换 |
| Operator | 数值前置条件、地址选择、输出正确性和真实完成 |

Native module 是受信任的进程内代码。Unchecked 执行移除数值认证和重复 grant assertion，保留有界解析与状态正确性。重复/过期事件通过 key/state 检查拒绝，不得重复减少计数；它们表示 backend protocol 错误。
