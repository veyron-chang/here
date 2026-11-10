#!/bin/bash
# 供 Windows 侧调用的小包装：避免 PowerShell 对 && 和引号的解析问题。
cd "$(dirname "$0")" || exit 1
mkdir -p out
bash ./boot_alpine.sh
echo "EXIT=$?"
