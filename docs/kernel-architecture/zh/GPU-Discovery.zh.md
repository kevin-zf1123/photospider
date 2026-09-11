# 有界 GPU 依赖发现

C++ `DependencyPhase::discover(capacity, candidates, compute)` 与 C
`ps_dependency_services_v9::discover` 在已供给输入上执行独立有界 discovery callback。
宿主通过既有 MemoryBudget 分配清零 native table，同步执行并 drain 后冻结、校验，再把
需求附加到当前 poll。不存在额外 page-fault executor 或任意 shader 自动追踪。

非空成功表要求返回 Need，宿主保留当前 Atomic 观察或完整 terminal RequestRecord 的
关联，再由既有 Run 取上游输入、supply 并继续 poll。有未解决请求时直接完成数值输出
会失败。pure block 内 discovery、discovery 内 block 及递归 discovery 均拒绝。空表不
增加依赖，可以接常量/control-only 完成。表指针只借用到 compute 返回；冻结后 native
写权限撤销。C 服务布尔 1 成功/0 失败，compute 返回普通结果码而非 NEED。C 子服务没有
发布、association、checkpoint 或 block。忽略错误仍然 sticky。

## 格式与资源

`PS_GPU_DISCOVERY_MSL_V8` 提供 `ps_discovery_emit`。16 字节 little-endian header
为四个 uint32：尝试次数、overflow、零、零。随后 capacity 个 144 字节 record：
uint32 port/roles/rank/零；uint64 offsets[8]；uint64 extents[8]。原子计数包括每次尝试；
超过 capacity 设置 overflow 且不越界写。callback 必须遵守声明的正数 candidates 上界，
包括重复和 overflow 尝试，最多 UINT32_MAX；宿主不隔离任意 shader 循环。

宿主核验 header/count、端口、rank、Data/Control/Validation 角色、正 extent、unused
零轴及全局域。每条原始 image row 先检查完整 C，两个半 channel row 无法拼接绕过。
错误记录失败；overflow 为 ResourceExhausted，不是空/部分成功依赖。

`maximum_gpu_requests` 默认及硬上限均为 65536，零禁用；它进入 Flight 身份。既有
work、stage、metadata 限额约束重复调用。表初始化/解码及 candidates 工作预扣，native
容量按真实分配分别取整后 admission。table/atlas/state/scratch/output owner 活跃时计费。

分组归一化共享 invocation 剩余工作。`Footprint::from_regions` 可返回 consumed_work，
在进入函数后对成功、失败和异常报告实际计费，decoder 在组间扣除。每 poll 的 discovery
metadata 跨调用共享，含 row/need/box 与 role 展开；各组只获得剩余 box grant。附加前
先限制原始 callback rows 并计费检索，去重不能消除已做工作。

## 验证

真实 C11 `discovery_plugin.c` 用 GPU 读取 Int64 control atlas，产生两个远距数据请求；
宿主 supply 后第二次 dispatch 读取 Float32 data atlas 并求值。整数控制避免 CPU/Metal
浮点寻址差异；负数/越域控制均失败。独立预期 8、24，两个观察四次 native dispatch。
control 修改后第一个结果为 24，data 边被替换，旧 data edit 不脏、新 data edit 脏该输出；
frozen 仍为 8。capacity1 overflow、禁用发现和提前完成数值均失败。128-entry 表逻辑
18448 字节、实际 native 32768 字节；当前 C 适配器最小 stage reservation 33108 字节成功，
33107 字节有限失败。运行命令见英文文档。

`test_gpu_discovery` 是明确非 native 的协议 mock，覆盖错误记录、共享工作/metadata、
raw row 限额、忽略错误、递归/block 误用、取消、terminal 完整 Q、半 C 和超 uint64
稠密容量的 rank8 域。static/shared 安装 consumer 运行 mock 并编译真实 C11 模块及 loader；
独立示例可消费安装包。无 native 时先验证 CPU，再返回 77，不作为 native 成功。

依赖模板中的同步 GPU producer 和 CPU 回退见 [Fragment Atlas](Fragment-Atlas.zh.md)。
