# 多输出与 Atomic 联合执行示例

`main.cpp` 仅使用已安装的 C++17 公开 API，包含四种 workflow 和独立数值 oracle。
构建、static/shared 独立安装命令见 [英文说明](README.md)。每次运行打印输出
shape、`node_id:output_index` 对应的实际执行次数、joint group 数量，以及每个
regional source 真正读取的 Region 集合。传输集合与逐输出依赖证书分开；证书在
`ExecutionResult::dependencies` 中可查询。最终应输出 `multi-output oracle=passed`。

- `420`：Y 为 3×5，Cb/Cr 为 2×3，匹配 long-double BT.709 与有效边缘 box oracle。
  Y-only 没有兄弟输出执行。可把 `WorkflowNodeOutput{1,"cb"}` 接入 HW field 算子。
- `split`：full/left/right 为 3×5×3、3×2×3、3×3×3，独立 ROI 精确对应源坐标。
  可修改 `split_x` 和 `PlanningOptions::output_regions`。
- `channels`：三组独立奇偶、非对称 kernel 与 anchor，逐样本精确匹配标量卷积。
  G-only 只读取 input0 和 input2；input1/input3 对 G 的 dirty 影响为空。
- `gaussian`：默认 radius=1.25、sigma=0.9，产生 5×5 kernel。通过公开
  `channel.extract` → `field.convolve` 精确复算 image R。Kernel-only 图像读取为零。

参数为 `--scenario all|420|split|channels|gaussian`、`--joint on|off`、
`--radius FLOAT`、`--sigma FLOAT`。Radius/sigma 必须有限且位于 `[0,64]`。
Radius 0、0.25、1、1.25、2、64 对应边长 1、3、3、5、5、129；sigma=0
保持尺寸并生成中心冲激。示例显式给予 100,000,000 单位依赖/work 预算，以运行
最大 kernel 及独立复算。radius>8 时图像与复算输出仅请求 `(1,2)` 像素，
仍生成并检查完整 kernel，以限制无结果保留示例中的重复上游观察。实际产品应根据图像尺寸选择预算；不足时返回错误。

选择单端口可以只保留 `WorkflowDocument::outputs` 中对应条目，也可对 frozen
plan 使用 `execute_fragments` 与 `DemandQuery`。端口与 shape 在编译时确定。
运行时变长端口、RequestRecord 联合执行不在本轮范围。独立安装消费者检查同时
运行 joint on/off，并继承 producer 的 sanitizer 设置。
