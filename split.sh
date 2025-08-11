#!/bin/bash
# split_photospider.sh
# 用法: ./split_photospider.sh photospider_all.cpp
# 会按 // FILE: <路径> 切分出文件并写入对应路径

set -e

if [ -z "$1" ]; then
    echo "用法: $0 <输入文件>"
    exit 1
fi

INPUT="$1"

current_file=""
mkdir -p out

while IFS= read -r line; do
    if [[ "$line" =~ ^//\ FILE:\ (.+)$ ]]; then
        # 新文件路径
        filepath="${BASH_REMATCH[1]}"
        current_file="$filepath"
        mkdir -p "$(dirname "$filepath")"
        echo ">> 输出文件: $filepath"
        # 清空新文件
        : > "$filepath"
    else
        if [ -n "$current_file" ]; then
            echo "$line" >> "$current_file"
        fi
    fi
done < "$INPUT"

echo "✅ 切分完成"

