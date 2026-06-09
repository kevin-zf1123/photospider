# 基准 Spike

这些基准 spike 必须在收紧 Metal context 访问策略或改变可移植 CPU 行对齐要求之前完成。

## Metal 适配器访问

比较 Metal 适配器 API 与直接解释后端 `context` 在以下场景中的表现：

- 16x16 和 64x64 tile
- copy、add、curve transform 等轻量操作
- 上传/下载密集工作负载
- 适配器开销可能影响 RT 延迟的混合 CPU/GPU 调度路径

在该基准产生数据之前，适配器 API 是推荐路径，直接访问 `context` 仍是实现敏感的逃生口。

## ARM Mac 对齐

在 Apple Silicon 上比较必需的 64 字节行对齐和可选 128 字节对齐模式：

- 按行 SIMD 内核
- 分块 HP 操作
- RT tile 更新
- 内存使用和 padding 开销

目前尚未记录基准结果。后续 OpenSpec change 应捕获实现细节、脚本、原始结果，以及 128 字节对齐是变为可配置项还是仅保留为实验的决策。

