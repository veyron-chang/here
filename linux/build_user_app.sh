#!/bin/bash
# 编译 Chapter 6 的 Linux 用户态程序。
#
# 为什么不用 `-static` 配 glibc：本机 /opt/riscv 只有 newlib 的
# riscv64-unknown-elf 工具链（裸机语义）。它的 crt0 不是 Linux 的 ecall 约定，
# 编出来的程序在 Linux 下会直接崩。所以这里用 -nostdlib 自己提供 _start。
#
# 如果你装了 gcc-riscv64-linux-gnu，可以用 CC=riscv64-linux-gnu-gcc 并去掉
# -nostdlib，那就变成标准 glibc 静态链接版本。
set -eu

HERE_DIR=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-riscv64-unknown-elf-gcc}
OUT=${1:-$HERE_DIR/out/user_app}

mkdir -p "$(dirname "$OUT")"

echo "==> CC = $CC"

"$CC" -nostdlib -static \
    -march=rv64gc -mabi=lp64d -mcmodel=medany \
    -Wl,-Ttext=0x10000 -e _start \
    -o "$OUT" "$HERE_DIR/user_app.c"

echo "==> built: $OUT"
riscv64-unknown-elf-readelf -h "$OUT" | grep -E -e 'Class' -e 'Machine' -e 'Type' -e 'Entry'
echo "==> 依赖的动态库（应为空）："
riscv64-unknown-elf-readelf -d "$OUT" | grep -i needed || echo "    (none)"
