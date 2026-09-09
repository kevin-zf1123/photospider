# 时域、机器学习与特殊图像数据

状态Proposed，D3覆盖与依赖规划。本篇不阻挡首期静态确定性算子，不声称现有内核已经具有序列/Deep/模型运行时。逐族落地前须升级为D1/D2专用规格。

## 时间数据

输入应有精确timestamp/timebase、有理帧率、source time、曝光区间、颜色状态、有效mask、切镜边界和缺帧政策。邻帧flow位移与px/s速度不同；向前flow与向后flow不能简单逐像素取负。序列可以按需提供，不能强制所有帧组成单个巨型Tensor常驻。

| ID / 功能 | 输入 → 输出 | 实现依赖与建议验收 |
| --- | --- | --- |
| TMP-01 sparse track | frames+points/regions→tracks/status/error | pyramid Lucas–Kanade等具名；已知translation、遮挡失效 |
| TMP-02 dense optical flow | frame pair+masks?→forward/backward flow/confidence | 非学习基线与模型分开；平移、motion boundary、brightness change |
| TMP-03 planar tracking | frames+plane ROI→homographies/validity | correspondence+鲁棒拟合；低纹理失败；与3D camera solve不同 |
| TMP-04 stabilize | track/motion sequence→smoothed transforms+crop | 估计/路径平滑/warp分开；平移锁定、保留intentional pan、空洞 |
| TMP-05 temporal denoise | time window+flow/noise/confidence→frame | motion compensated与直接median分开；noise下降、ghosting、texture |
| TMP-06 deflicker | frames+measurement ROI→per-frame correction+frames | 全局亮度、局部闪烁与真实灯光区分；cut/reset、渐变场景 |
| TMP-07 time remap | source+time function→frames | nearest/frame mix/flow interpolation；端点、缺帧、time精度 |
| TMP-08 interpolate | two frames+t+flow/model→frame | t=0/1原帧，occlusion与cut策略，运动多解 |
| TMP-09 shutter integration | source samples/flow+shutter→blurred frame | 秒/degree转换、center/bias、weight；匀速轨迹与常量 |
| TMP-10 echo/temporal reduce/difference | frame times+weights→frame/statistics | 显式时刻列表，sum/median/min/max等 |
| TMP-11 time displacement | map[x,y]的源时间+sequence→frame | 每像素动态时间需求；时间插值与空间插值各定义 |
| TMP-12 rolling shutter | frames/motion+row exposure model→frame+validity | per-row时间模型、几何校准与估计分开 |
| TMP-13 deinterlace | field sequence+field order→progressive frames | top/bottom、时间相位、motion模式；静态细线/运动齿形 |
| TMP-14 temporal restoration | frames+defects/flow→repaired frames | dust/scratch/deblock/occlusion-aware patch；不假装单帧信息充分 |

Nuke Time Nodes与AE Time Effects公开了不同重定时、运动及时间位移功能；它们映射到不同源时间需求，不宜统一成“读前后两帧”。[^time][^ae] OpenCV光流可作为CPU确定性基线候选，纹理/遮挡条件仍需明确。[^flow]

CPU/GPU先按每个算法的数值与内存协议实现；窗口长度、状态寿命、取消与缓存身份必须包括时间和输入快照。状态模型需reset、随机访问和重放定义；帧缓存由宿主/内核已接受职责管理，算子不能自行新建持久任务系统。

## Deep 与辅助数据

| ID / 功能 | 数据与输出 | 关键语义/验收 |
| --- | --- | --- |
| AUX-01 AOV select/transform | named depth/normal/position/motion/ID/coverage→AOV | data角色，不做颜色transfer；normal变换/normalize，整数ID保持 |
| AUX-02 Cryptomatte extract | ID/coverage channels+manifest+selection→mask | hash编码、重叠、AA、名称查找与缺manifest；不能用RGB色键替代 |
| AUX-03 deep select/sort | offsets+samples→deep | front/back depth、surface/volume、排序tie、最大sample数 |
| AUX-04 deep merge/holdout | deep inputs→deep | 交错深度、重叠体积、opacity区间规则；空像素表示G3 |
| AUX-05 deep flatten | deep→flat color/alpha/depth | 给定view/样本解释与积分；透明层与终止条件 |
| AUX-06 geometric attributes | depth/normal/position+camera→derived data | reconstruct position、normal、depth compare、relight输入；无数据不猜单位 |

OpenEXR提供Deep、多部分、多通道存储，但读得进文件不代表已经有Deep合成算法。[^exr] Cryptomatte有独立编码和manifest规范。[^crypto] 深度Z可能是轴向坐标而非欧氏距离，必须转换后才可接光学/几何流程。

## 模型推理任务

| ID / 功能 | 推理输入 → 输出 | 特有验收 |
| --- | --- | --- |
| ML-01 semantic/instance/prompt segment | image+points/box/text?→labels/instances/masks/confidence | 提示坐标、细目标、域外类别；confidence不当alpha |
| ML-02 general/portrait matting | image+trimap?→alpha/F? | 发丝、透明、颜色污染，区别硬分割 |
| ML-03 depth/normal | image+camera?→relative/metric geometry+confidence | 尺度歧义，单目relative不标米 |
| ML-04 denoise/deblur/superres | image/burst+noise/scale→image | 真细节/幻觉、分块接缝、训练域；与插值resize不同 |
| ML-05 RAW enhance/demosaic | CFA/calibrated sensor→RGB/RAW | 支持相机/CFA、黑白电平与域迁移 |
| ML-06 inpaint/remove/outpaint | image+edit mask+condition→candidates | mask外保持、seed、参考依赖和多解性 |
| ML-07 relight/intrinsic/face | image+geometry/semantic inputs→image/layers | 预测几何与实测分开，保留中间输出 |
| ML-08 reflection remove/colorize/restore | image+reference?→image/confidence | 不称唯一恢复真实原图；reference与重建假设 |
| ML-09 learned flow/interpolation/video masks | frames+state/prompt→flow/frame/matte/state | temporal consistency、cut reset、random access |
| ML-10 adaptive LUT/local color | low-res image→LUT weights/affine grid→full image | 输入颜色域、basis、grid、slice定义；固定模型与数据影响缓存 |

模型接口必填：model/weights版本、来源与许可、layout/dtype/range/color、尺寸/padding、预处理、输出协议、tile overlap/receptive field、precision/backend、memory、seed和状态。固定seed不保证跨设备/版本逐值相同。模型runtime许可不自动授予权重和训练数据的使用权。

训练输入是样本/标签或paired targets、训练配置与初始化权重，输出是模型产物及独立验证结果；推理输入是新数据和固定模型，输出是上述任务结果。训练不作为普通图像op的隐式副作用。Nuke CopyCat/BigCat与Inference在产品中也分离：训练产生模型，Inference应用模型。[^air]

训练/验证应按场景、镜头、主体等相关性分割，相邻帧随机拆分可能泄漏。恢复任务关注已知真值误差、偏差与细节；生成任务另评语义、边界和一致性，不能用“看着合理”证明像素恢复准确。

Image-Adaptive-3DLUT、HDRNet与Colorful Image Colorization分别提供全图自适应LUT、局部affine grid、上色的原始方法例子，适合保持中间表示以复用确定性后处理。[^ml]

## 来源

[^time]: Foundry，[*Time Nodes*](https://learn.foundry.com/nuke/content/reference_guide/time_nodes/time_nodes.html)，滚动指南。
[^ae]: Adobe，[*Time Effects*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/time-effects.html)，滚动英文指南。
[^flow]: OpenCV，[*Optical Flow*](https://docs.opencv.org/4.x/d4/dee/tutorial_optical_flow.html)，4.x官方教程。
[^exr]: OpenEXR，[*Technical Introduction*](https://openexr.com/en/latest/TechnicalIntroduction.html)，Deep与数据角色。
[^crypto]: Psyop，[*Cryptomatte specification and implementation*](https://github.com/Psyop/Cryptomatte)，仓库访问版本1.4.0。
[^air]: Foundry，[*CopyCat*](https://learn.foundry.com/nuke/current/content/reference_guide/air_nodes/copycat.html)、[*BigCat*](https://learn.foundry.com/nuke/content/reference_guide/air_nodes/bigcat.html)、[*Inference17.0*](https://learn.foundry.com/nuke/17.0/content/reference_guide/air_nodes/inference.html)。
[^ml]: Zeng等，[*Image-Adaptive-3DLUT*](https://github.com/HuiZeng/Image-Adaptive-3DLUT)，TPAMI2022；Gharbi等，[*HDRNet*](https://groups.csail.mit.edu/graphics/hdrnet/)，SIGGRAPH2017；Zhang等，[*Colorful Image Colorization*](https://richzhang.github.io/colorization/)，ECCV2016。
