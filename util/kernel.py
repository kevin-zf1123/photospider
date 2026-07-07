import numpy as np
import cv2

# Define the kernel values as a 32-bit floating-point array
# kernel_data = np.array([
#     [0.0,  -0.25, 0.0],
#     [-0.25, 1.0, -0.25],
#     [0.0,  -0.25, 0.0]
# ], dtype=np.float32)
kernel_data = np.array([
    [-1, 0, 1],
    [-2, 0, 2],
    [-1, 0, 1]
], dtype=np.float32)
gain = 1
kernel_data = kernel_data * gain
# Define the output filename
# It's important to use an extension that supports floats, like .tif or .exr
output_filename = "assets/differential_kernel.tif"

# Save the array as a single-channel, 32-bit float image
cv2.imwrite(output_filename, kernel_data)

print(f"Successfully created kernel image: '{output_filename}'")
import tifffile as tiff

# 读取 tiff 文件
data = tiff.imread("assets/differential_kernel.tif")

# 查看数据的类型和值范围
print("数据类型:", data.dtype)
print("最小值:", data.min())
print("最大值:", data.max())

# 判断是否包含负值
if (data < 0).any():
    print("包含负值")
else:
    print("不包含负值")