# 鱼眼、相机投影与曲边光栅化

状态Proposed。已知源/目标模型的2D重投影为D1数学核心、D2接口；三维曲边光栅化为D3独立能力。用户引用的《光栅化映射证明》已读取，下面保留可成立的推导并明确退化条件；该对话不作为已实现或商业兼容证据。

## 二维鱼眼算子

建议`projection.make_map(source,target,rotation,canvas)→inverse map+validity`，再调用GEO-06。源必须有投影/FOV/center/scale/内参，不能从普通图片自动得知镜头模型。缺失的视角内容不能通过warp恢复。

对外角度为degree，以下所有公式的theta均为内部radian：`theta=theta_degree*pi/180`，标定多项式系数也按radian幂解释。FOV显式指完整开角，例如圆形FOV180°对应最大离轴角90°。equidistant/equisolid到反极点时仍有方位角退化，必须限制chart或标记seam/invalid。

| ID / 模型 | 半径r与离光轴角theta | 合法性与验收 |
| --- | --- | --- |
| PRJ-01 perspective | f·tan(theta) | 可见前半球受限，theta接近pi/2发散 |
| PRJ-02 stereographic | 2f·tan(theta/2) | theta→pi发散，选定chart |
| PRJ-03 equidistant | f·theta | 角度与半径成正比 |
| PRJ-04 equisolid | 2f·sin(theta/2) | 等立体角面积关系，尺度固定 |
| PRJ-05 orthographic | f·sin(theta) | 超pi/2不单调，inverse范围须限 |
| PRJ-06 calibrated fisheye | theta_d=theta(1+k1theta²+k2theta⁴+k3theta⁶+k4theta⁸) | 内参/畸变、FOV单调与逆解残差 |
| PRJ-07 spherical charts | equirectangular/cube/dual stereographic→map | seam、多面footprint与坐标方向 |
| PRJ-08 nonlinear mesh rasterize | mesh+camera+attributes→color/depth/coverage | 曲边、visibility、depth与属性插值；D3 |

理想模型与广角标定可对照Kannala–Brandt原论文；OpenCV公开的fisheye多项式是具名模型，不能等同全部真实鱼眼镜头。[^camera][^opencv]

## 球极投影的推导与限制

以相机看向+z，rho=|P|，投影极为(0,0,-1)：

```text
u = x/(rho+z), v = y/(rho+z)
q(u,v) = (2u, 2v, 1-u²-v²)/(1+u²+v²)
P = rho*q(u,v)
```

这是r=tan(theta/2)的无量纲球极chart，与上表2f尺度可换算。屏幕坐标只依方向，所以相同2D点对应同一正向射线；投影包含sqrt且一般把直线变圆弧，不能用单个4×4齐次投影矩阵精确表示。

有限径向区间要求`0<n<far<∞`、rho∈[n,far]，可单独采用log depth：`zeta=2*log(rho/n)/log(far/n)-1`。这不改变2D圆弧。直接对复屏幕坐标u+iv取log则改变曲线形式，并引入中心发散和角分支。

角圆盘除以两个轴向尺度只得到椭圆盘，不能自动填满矩形。欲得到矩形chart的体积参数域，需要选矩形chart对应的球面方向范围，或增加独立盘到方形映射。完整球壳需要seam或多个chart，不能宣称单个无缝无奇点普通长方体表示。

对两端点对应q0、q1，令n=q0×q1。其大圆平面n·q=0代入逆映射，得到：

`2nx*u+2ny*v+nz*(1-u²-v²)=0`。

nz≠0为圆，圆心(nx/nz,ny/nz)，半径`sqrt(1+(nx²+ny²)/nz²)`；nz=0为退化直线。但q0=q1、q0=-q1、线段经过原点、碰投影极点时不能直接用此式唯一选择可见弧段。屏幕两个端点还需对应所选3D线段的方向区间与裁剪规则。

端点bbox不足：端点(.5,.5)、(-.5,.5)的相应小弧通过(0,.6180339887498949)，超出端点y=.5的框。需加入弧极值与采样footprint。这一数值是公式推导的反例，不是GPU运行结果。

## 三维光栅化另需的规格

若后期实现PRJ-08，需要mesh topology、每顶点属性、相机/投影、near/far、backface、coverage与depth数据。候选方法是曲边保守bbox/隐式edge test后，用像素反投影射线与三角形平面相交，求真实重心属性和可见深度；或按像素误差自适应细分走标准栅格。

验收必须覆盖共享边一致、曲线极值、正深度、遮挡、穿原点/极点、背面、深度精度及透视属性插值。一般二维鱼眼滤镜不需要引入完整mesh renderer；其目标是已知源画面的重采样。

## 引用材料的证据状态

已实际尝试读取 [ShaderToy wcGBDD](https://www.shadertoy.com/view/wcGBDD) 和 [s3XGz4](https://www.shadertoy.com/view/s3XGz4)，均未取得源码，工具返回无法打开。保留为待核验参考，不描述其shader算法、不声称与它们兼容。以后取得源码后应核对坐标、许可、投影模型、边界与采样，独立重建fixture。

本篇2D验收是方向→投影→逆方向的angular error、calibrated点的reprojection error、identity和跨缝图；CPU Float64几何作参考，GPU Map/Sampler分别验证。任意projection的输入ROI仍受G4约束。

## 来源

[^camera]: Kannala、Brandt，[*A Generic Camera Model and Calibration Method for Conventional, Wide-Angle, and Fish-Eye Lenses*](https://users.aalto.fi/~kannalj1/calibration/Kannala_Brandt_calibration.pdf)，PAMI2006。
[^opencv]: OpenCV，[*Fisheye camera model*](https://docs.opencv.org/4.13.0/db/d58/group__calib3d__fisheye.html)，4.13.0。
