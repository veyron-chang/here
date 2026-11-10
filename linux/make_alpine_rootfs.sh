#!/bin/bash
# Chapter 6 实践 2：把 Alpine Linux minirootfs 做成 VirtIO-Block 上的 rootfs。
#
# 全程免 root：
#   - tar 用 --no-same-owner 解压（Uid/Gid 变成当前用户，mke2fs -d 会沿用）
#   - mke2fs -F -d 直接从目录建 ext4，不需要 mount
set -eu

HERE_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=$HERE_DIR/linux/out
TARBALL=${TARBALL:-/mnt/c/Users/veyro/Downloads/alpine-minirootfs-3.24.4-riscv64.tar.gz}
IMG=${IMG:-$OUT_DIR/alpine_rootfs.img}
SIZE_MB=${SIZE_MB:-256}
ROOTDIR=$OUT_DIR/alpine_rootdir

mkdir -p "$OUT_DIR"

if [ ! -f "$TARBALL" ]; then
    echo "error: 找不到 $TARBALL" >&2
    echo "       从 https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/riscv64/ 下载" >&2
    exit 1
fi

echo "==> 解压 $TARBALL"
rm -rf "$ROOTDIR"
mkdir -p "$ROOTDIR"
tar -xzf "$TARBALL" -C "$ROOTDIR" --no-same-owner 2>/dev/null || tar -xzf "$TARBALL" -C "$ROOTDIR"

echo "==> 改动前的串口 / 登录配置"
grep -n 'ttyS0' "$ROOTDIR/etc/inittab" || true
grep -n '^root:' "$ROOTDIR/etc/shadow" || true

# 1) 启用串口 getty。Alpine 默认把这行注释掉了。
sed -i 's|^#ttyS0::|ttyS0::|' "$ROOTDIR/etc/inittab"

# 2) securetty 允许 root 从 ttyS0 登录
if [ -f "$ROOTDIR/etc/securetty" ]; then
    grep -qx 'ttyS0' "$ROOTDIR/etc/securetty" || echo 'ttyS0' >> "$ROOTDIR/etc/securetty"
fi

# 3) root 设为空密码：minirootfs 默认是锁定状态（!），登录会失败
sed -i 's|^root:[^:]*:|root::|' "$ROOTDIR/etc/shadow"

# 4) fstab 里根分区写成 /dev/vda，避免 OpenRC 报错
cat > "$ROOTDIR/etc/fstab" <<'EOF'
/dev/vda        /               ext4    defaults,noatime 0 1
EOF

echo "==> 改动后的串口 / 登录配置"
grep -n 'ttyS0' "$ROOTDIR/etc/inittab"
grep -n '^root:' "$ROOTDIR/etc/shadow"

echo "==> 生成 ext4 镜像（${SIZE_MB} MiB）"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
mke2fs -q -t ext4 -F -d "$ROOTDIR" "$IMG"

echo "==> image: $IMG ($(stat -c %s "$IMG") bytes)"
debugfs -w -R 'ls -l /etc/inittab' "$IMG" 2>/dev/null | cat
debugfs -w -R 'stat /sbin/init' "$IMG" 2>/dev/null | cat
