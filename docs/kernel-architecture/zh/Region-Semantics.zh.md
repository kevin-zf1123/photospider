# Region 语义

## 模块边界与职责 (Scope & Ownership)

`Region` 描述完整张量描述符中的逻辑样本覆盖范围。存储 origin、byte offset、有符号 stride、padding 和物理 planar 页描述样本的存储方式，不改变 Region。规划器授权依赖，执行器传递精确需求，存储 owner 限定可读和可写样本。

## 核心数据结构与内存布局 (Data Layout & Memory)

```cpp
#include <cstdint>
#include <vector>

#include "photospider/data/region.hpp"

ps::Region whole_region(const std::vector<std::uint64_t>& shape) {
  return ps::Region::whole(shape);
}
```

每个维度是描述符轴内的半开区间 `[offset, offset + extent)`。构造函数验证维度和经过溢出检查的端点。形状为 `(height, width, channel)` 的张量可请求像素矩形，同时保留每个像素的四个 RGBA 通道样本。Planar storage 可以把通道放在不同平面；逻辑通道完整不表示物理字节交错连续。

输入 view 另外提供 storage origin、offset、有符号 stride、有效 coverage 和 callback demand。回调只能用给定映射访问授权需求与有效 coverage 的交集内样本。输出发布记录其描述符和实际产出的精确 Region。空 coverage 不授权读取样本。

## 调度与状态机 (Execution & State)

规划器根据 operation 声明的规则推导依赖。Whole operation 需求完整输入；Elementwise 按坐标映射；Halo 扩展需求并在完整图像边界裁剪，运算使用溢出检查；Shrink 把 ceil-div 输出坐标映射为裁剪后的输入 box。如果图像端口要求全通道，未知端口或轴、越界矩形和部分通道需求都会被拒绝。

规划选项为每个输出名称保留精确 Region 和正数 tile 尺寸。修改任一选项都会重新规划优化后的 IR。即使多个名称指向同一节点，名称和 Region 仍进入 plan identity；运行时字节不进入 identity。`tile_plan` 为一个 Region 推导依赖裁剪子计划，不必分配完整 tile 网格。相邻 tile 可能重复计算重叠 halo。Whole、非确定性和有副作用边界按拓扑顺序一次性物化完整结果。

Regional Value executor 返回请求 coverage；没有请求时返回完整输出。普通 dense Value binding 提供完整快照；`RegionalSource` 在 Run 内复制其 metadata 与 callable，并填充宿主提供的 packed region 存储。它必须支持并发不可变读取、观察协作式取消，并准确返回被写入的 Region。source 和 operation callback 不得在同一 context 自有 worker 上同步重入执行。

Value 收集会打包请求 coverage，同时保留完整逻辑描述符。结构化 planar 收集使用不同路径，要求 `PlanarImage` binding 的描述符、facet、layout 和 tile 几何匹配编译声明。Planar 输入拒绝 Value、`RegionalSource` 和 snapshot binding。`execute_stream` 支持 Value sink；若计划要求结构化 planar 执行则返回 `TypeMismatch`。参见 [数据模型](Data-Model.zh.md) 与 [编译和执行](Compiler-and-Execution.zh.md)。

`execute_stream` 在 execute 调用线程同步调用 sink，并传入借用的 `ValueView`；view 在 sink 返回时失效。符合条件的确定性、无副作用 CPU dependency stream 可在交付前端 tile 的同时准备有界数量的后续 tile；窗口受执行并行度限制。Sink 调用仍按序进行；sink 阻塞会停止后续交付，但已准入窗口中的工作可以完成。Sink 失败会停止后续交付并排空已准入工作；已交付 tile 无法撤销。收集式执行失败时不返回部分结果。

取消和 currentness 检查覆盖准入、callback 入口与结束、sink 调用和最终组装。入口处 Stale 优先于 binding 校验；进入后 Cancelled 优先于 Stale 和普通错误。所有已准入 callback 退出后，宿主才复用借用的 source/output storage。

## 算法与数学实现 (Algorithms & Math)

对正数 tile 大小 `T` 和 extent `E`，规划器以 checked ceil-div 计算 tile 数：

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

整数表达式避免 `E + T - 1` 溢出。分配前检查边界与形状乘积。空 extent 不产生样本工作。数值参数 schema 可声明有限闭区间；有界 Int64 端点保持在 binary64 精确整数范围，有界 Float64 端点必须有限。Halo operation 可从必需的有界 Int64 参数解析 radius。

## 限制与非目标 (Limitations & Non-Goals)

- 如果 operation 声明需要完整输入，regional 执行不会把它变成局部 operation。
- 收集式外层 tile 遍历顺序执行。符合条件的确定性 CPU dependency stream 使用有界并发 tile 窗口；其他 stream 和 regional 路径遵循各自的执行方式。
- caller-owned 输入 payload 不计入 computation `maximum_live_bytes` 子限额。启用 managed resource root 时，外部 Value storage 另按 Referenced 准入；source 私有外部状态、线程栈和进程 RSS 不计入受控内存上界。
- 流式 sink 在最终成功前收到的 tile 不可撤销。需要回滚时调用方必须自行暂存输出。
- `RegionalSource` 是 C++ execution binding，不增加 source codec 或 provider ABI 扩展。
