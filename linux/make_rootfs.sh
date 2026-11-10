#!/bin/bash
# 制作 VirtIO-Block 后端使用的 ext4 镜像，并把 user_app 注入进去。
#
# 全程免 root：
#   - `mke2fs -d <dir>` 直接从目录生成文件系统，不需要 mount
#   - `debugfs -R 'ls -l /'` 不需要 mount 就能校验内容
set -eu

HERE_DIR=$(cd "$(dirname "$0")" && pwd)
OUT_DIR=$HERE_DIR/out
IMG=${1:-$OUT_DIR/block_image}
SIZE_MB=${2:-32}
ROOTDIR=$OUT_DIR/rootdir

mkdir -p "$OUT_DIR"

# 1) 编译用户态程序
bash "$HERE_DIR/build_user_app.sh"

# 2) 准备根目录内容
rm -rf "$ROOTDIR"
mkdir -p "$ROOTDIR"
cp "$OUT_DIR/user_app" "$ROOTDIR/user_app"
chmod 0755 "$ROOTDIR/user_app"
echo "hello from the host filesystem" > "$ROOTDIR/host_note.txt"

# 3) 造一个全零镜像，再用 -d 直接把目录内容写进去
#    注意：必须加 -F，否则 mke2fs 会因为 block_image 不是块设备而交互确认。
#    注意：不要给 mke2fs 传块数参数，否则镜像尺寸会被重设成那个值。
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
mke2fs -q -t ext4 -F -d "$ROOTDIR" "$IMG"

echo "==> image: $IMG ($(stat -c %s "$IMG") bytes)"
echo "==> 免挂载校验根目录："
debugfs -w -R 'ls -l /' "$IMG" 2>/dev/null | cat
