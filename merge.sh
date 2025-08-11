#!/bin/bash
# merge_photospider.sh
# 用法: ./merge_photospider.sh
# 会把 include/ src/ cli/ examples/ 下的 .hpp 和 .cpp 文件按路径合并到 tmp2.txt

OUTPUT="tmp2.txt"
> "$OUTPUT"  # 清空输出文件

# 遍历目录
for dir in include src cli examples; do
    if [ -d "$dir" ]; then
        find "$dir" -type f \( -name "*.hpp" -o -name "*.cpp" \) | sort | while read -r file; do
            echo "// FILE: $file" >> "$OUTPUT"
            cat "$file" >> "$OUTPUT"
            echo "" >> "$OUTPUT"  # 文件间加空行
        done
    fi
done

echo "✅ 已生成合并文件: $OUTPUT"

