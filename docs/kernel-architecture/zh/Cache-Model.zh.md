# 缓存模型

## 模块边界与职责 (Scope & Ownership)

Photospider 分别管理编译计划、不可变输入快照、已完成 CPU/GPU 结果、native 输入副本、共享中的计算和可丢弃磁盘结果。缓存 entry 保留不可变 owner；清除或淘汰只撤销缓存资格，活动 reader 仍持有原 owner 与资源租约。

`PlanCacheKey` 描述物理计划 identity，不包含输入 payload，也不检查图是否过期或标识计算结果。结果缓存由 `ExecutionContextConfig::result_cache_bytes` 显式启用，并受 context 托管 live-byte 限额约束。零值不保留已完成结果。

## 核心数据结构与内存布局 (Data Layout & Memory)

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Value& value) {
  return store.import_value(value);
}
}
```

`InputSnapshotStore` 将受支持内建 dtype 的 rank 1 至 rank 8 Value 存入不可变分块。Generic 导入精确保留有效原始位；支持的 typed scalar/tensor 值会校验语义样本。携带 image、image-plane、mask 或 rank 至少为 3 的 ColorArray identity 的 Value 必须使用结构化 planar storage，snapshot import 会拒绝它们。`maximum_blocks` 限制每个版本的目录项数；`maximum_bytes` 限制所有版本保留的实际 payload。目录 metadata 有独立上限。调用方传入的 Value 与 snapshot metadata 不计入该字节限额。

Patch 要求 dtype、shape 和 facets 匹配，并替换一个精确非空 Region。它只复制相交 block，保留旧版本。Snapshot access options 为导入、读取、hash 和受影响 block 复制提供取消与样本上限。取消读取可能只填充调用方 buffer 的一部分；成功才表示完整读取有效。

`content_identity(region)` 使用规范 framing 对 dtype、shape、请求坐标、facets 和精确样本位进行 hash。分块几何、origin、stride 和 allocation 不影响 identity；正负零和不同 NaN payload 保持区分。Snapshot binding 用 Run 取消 token 提供 regional read。Snapshot/session identity 只表示来源；确定性 operation 不能从其字符串推导值或依赖。

## 调度与状态机 (Execution & State)

Memory result 命中会重新验证解析后的 descriptor、需求 coverage、输出语义规则和 typed 样本，包括共享 flight 完成后的并发查找。数值校验设置所需浮点环境并恢复调用方环境。Generic Drop 输出可携带不透明 facet；从该边界开始的 PreserveInput 链保留此能力，但 typed 声明仍需已证明 facet。非法计算 typed 值返回 `OperationFailed`。

Regional result key 递归覆盖所需 producer Region、operation 语义与参数，以及稳定 source content。无关图编辑和节点 ID 不会使相同内容失效；Whole 依赖采用保守策略，scalar 变化会使相关输出失效。未证明的 generic input 仍可执行，但会禁用其后代的跨 Run 结果复用。只有确定性、无副作用、声明 cacheable 且实现身份已证明的工作可复用。

小型、经过 preflight 验证、offset 为零的 dense Value，在推导需求覆盖完整 Value 且不超过 2048 bytes 时可建立 compact content identity。Snapshot、bounded scalar 和 compact whole-Value source 使用独立 key 类别。更大或局部 direct input 未证明稳定。磁盘缓存资格更严格，只覆盖通过校验的 Float32 Value 输出，且 facet 必须是唯一有效的 image 或 coverage-mask semantic。结构化 planar executor 不走 Value result/disk cache。

有界共享 coordinator 合并相同的 in-flight regional computation。CPU 工作运行在 context callback pool 中。每个等待者独立处理自己的 cancellation 和 currentness；最后一个订阅者取消后，producer 排空才返回。Producer snapshot 自行持有输入和 registry，不依赖 caller stack 或可编辑图。

`FrozenExecution` 捕获当前计划及不可变 Value/snapshot binding。图被替换或销毁后，frozen 对象仍有效。Capture 会复制 snapshot handle，避免调用方后续替换 handle 改变冻结输入。`for_region` 派生固定输出 tile。自定义 `RegionalSource` 必须先导入再 freeze；普通计划执行仍检查 stale。

DemandHandle 和 fragment execution 也可在同一 context pixel LRU 中保留精确依赖 observation。Manifest 保存结构链接、传递 Data/Control/Validation footprint、内容 identity 与 fragment key，不保存 input、snapshot 或 pixel owner。复用前须匹配包括正负 control 证据在内的 source bytes。每个 fragment key 包含实际逻辑 Region；所有片段必须仍在当前 cache epoch 才能命中。清 cache 后旧 producer 不能重新填充该 epoch。

`maximum_dependency_cache_metadata` 默认 65536 proof units，范围为 1..1048576；每个 manifest 按 distinct record owner、row/tag/coordinate storage 和 source witness 计数，同一 manifest 内共享 owner 只计一次。`maximum_dependency_cache_work` 是独立的可选 per-Run work 上限，默认 1048576；设为零会禁用依赖缓存复用。proof 超限会跳过复用或保留，继续执行计算。去重不抵消已经发生的遍历、hash、关联和规范化工作。

## 算法与数学实现 (Algorithms & Math)

有界磁盘缓存使用规范 SHA-256 key，以及默认 operation registry 的实现 fingerprint。它只保存符合资格的 Float32 `Value` 区域。每条记录在发布前校验 dtype、shape、Region、facet key/payload、字节数、checksum 和结果 key。读取分配大小来自已验证计划，不来自文件字段。损坏、截断、不匹配或不支持的记录按可丢弃 miss 处理。

写入先完成临时文件，再 rename；该缓存不提供持久恢复保证。队列压力或写入失败只跳过保留，不使已计算结果失败。待写项持有已计费源 buffer，在计算压力下可丢弃。一个 context 独占配置目录；其他文件会被忽略。`flush_disk_cache()` 等待排队写入；`clear_disk_cache()` 删除 entry 并使待写 epoch 失效。

## 限制与非目标 (Limitations & Non-Goals)

- 磁盘缓存需显式配置，且内存 result-cache 容量必须为正。它是本地可丢弃存储，不是持久结果库。
- 持久实现身份仅对默认 registry 提供。自定义和 C module registry 可使用进程内缓存，但没有持久实现身份。
- 只有编译计划的 `execution_mode` 为 `CpuExact` 时才读写磁盘缓存。计划选择 native GPU 后，即使 backend 选择最终回退到 CPU，也不具备磁盘缓存资格；native GPU 结果仍与 CPU-exact key 隔离。
- 回退结果及其后代不写入 native key；近似 native 结果不能替代 exact CPU 结果。
- Native 输入副本 key hash 实际需求样本字节、descriptor、facets、Region 和 device identity，可避免不可变普通 Value 重复上传，但不会让任意 `RegionalSource` 获得计算结果缓存资格。
- 清除与淘汰不会使活动借用数据失效。FrozenExecution 是内存 owner，没有序列化 frozen-plan reader。

公开 API 与行为检查见 `include/photospider/data/input_snapshot.hpp`、`tests/unit/test_input_snapshot.cpp` 和 `tests/integration/test_frozen_execution.cpp`。结构化 planar 输入工作流见 `tests/integration/test_planar_image_workflow.cpp`；该入口验证 planar binding，不表示 planar page 支持 snapshot 或 result cache。
