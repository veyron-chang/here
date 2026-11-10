#!/bin/bash
# 清理所有残留的模拟器进程与 FIFO，并做语法自检
pkill -f 'target/release/here' 2>/dev/null
pkill -f 'boot_alpine.sh' 2>/dev/null
pkill -f 'run_alpine.sh' 2>/dev/null
sleep 1
rm -f /tmp/here_alpine.fifo

echo "--- 残留模拟器进程 ---"
pgrep -a -f 'target/release/here' || echo "无"

echo "--- 脚本语法检查 ---"
if bash -n "$(dirname "$0")/boot_alpine.sh"; then
    echo "SYNTAX_OK"
else
    echo "SYNTAX_FAIL"
    exit 1
fi

echo "--- 镜像 ---"
ls -l "$(dirname "$0")/out/alpine_rootfs.img"
