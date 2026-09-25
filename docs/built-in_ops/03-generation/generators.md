# 图形、坐标、渐变与噪声生成

2026-09-26：[D01–D12英文权威定稿](decisions.md)已获维护者接受，
[中文对照](zh/decisions.zh.md)供阅读。本文为导航，不表示运行时已实现。
旧内核测试实现需要按调用者迁移后退役，不作为新规格的兼容默认。

[候选目录](catalog.md)保留GEN-01..08与NOI-01..10及成员来源，
[技术门禁](freeze-gates.md)列出注册前需完成的具体设计和验证。

- 执行按成员依赖采用Regional、Halo或Whole；支持部分GPU实际负载，profile按后端区分。
- 图像颜色遵循完整FMT描述、straight和显式alpha；渐变保留原始色相及圈数。
- 周期坐标正常舍入，舍入到上端点不再wrap或nextDown。
- 双圆选最大有效根；无可返回坐标默认valid=0，相同圆为非法输入。
- GEN-07的三类mesh均为通用数值插值，不识别颜色或alpha，相关处理通过显式workflow组合。
- 随机采用Philox4x64-10；坐标signed32，frame/draw/stream为32-bit非负范围。
  完整packing及浮点映射尚待冻结，不能使用附件旧Philox4x32序列作新验收。
- 点集保留有限候选拒绝与FIFO扩展；max_count为正常停止目标，资源预算独立。
- 通用rank lookup可先实现，具名blue/STBN仍须生产资源和质量批准。
- shot、speckle、linear RGB grain分开；grain显式选择none/sum/l2，strength仅为最终乘数，
  隐藏RGB同样处理，alpha按位复制，不自动裁剪。

具体输入输出、成员边界和首批范围在对应技术门禁完成后冻结。
不将源草案的成熟度、示例或统计报告当作实现证据。
