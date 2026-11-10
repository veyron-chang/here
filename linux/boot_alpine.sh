#!/bin/bash
# Chapter 6 实践 2：从 initramfs 手动 switch_root 到 VirtIO-Block 上的 Alpine rootfs。
#
# 为什么不用内核的 root= 自动挂载：
#   预编译内核内建了 busybox initramfs。用 rdinit=/no-such-path 可以让内核走到
#   prepare_namespace()，但 mount_block_root() 最终返回 -2(ENOENT)：
#
#     [ 17.834636] check access for rdinit=/no-such-path failed: -2, ignoring
#     [ 17.835276] VFS: Cannot open root device "254:0" or unknown-block(254,0): error -2
#     [ 17.835622] fe00          262144 vda
#     [ 17.835686]  driver: virtio_blk
#
#   设备明明存在（254:0），root=/dev/vda 和 root=254:0 都试过，都是 -2。
#   这条路依赖内核内部对 rootfs 的假设，继续深挖性价比很低。
#
# 所以改成「initramfs 里手动交接」，这也是真实发行版 initramfs 的标准做法：
#   1. 挂 devtmpfs（否则没有 /dev/vda）
#   2. 挂 /dev/vda 到 /mnt
#   3. 把 devtmpfs / proc / sys 挂到新根下（否则 switch_root 后没有 /dev/console）
#   4. exec switch_root /mnt /sbin/init
set -u

HERE_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=$HERE_DIR/linux/out
KERNEL=${KERNEL:-/mnt/c/Users/veyro/Downloads/linux-busybox-rv64gc.bin}
DTB=${DTB:-$HERE_DIR/dts/virt.dtb}
IMG=${IMG:-$OUT_DIR/alpine_rootfs.img}
EMU=${EMU:-$HERE_DIR/target/release/here}
LOG=${LOG:-$OUT_DIR/alpine.log}
MAX_CYCLES=${MAX_CYCLES:-60000000000}
FIFO=${FIFO:-/tmp/here_alpine.fifo}

for f in "$EMU" "$KERNEL" "$DTB" "$IMG"; do
    if [ ! -e "$f" ]; then
        echo "error: 缺少 $f" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"
: > "$LOG"
rm -f "$FIFO"
mkfifo "$FIFO"

echo "==> kernel : $KERNEL"
echo "==> dtb    : $DTB"
echo "==> rootfs : $IMG"
echo "==> log    : $LOG"

# 注意：dts 里声明了两个 virtio 节点，所以必须挂两个设备，
# 否则内核 probe 空槽位会访问异常 panic。
exec 9<> "$FIFO"
"$EMU" "$KERNEL" --dtb="$DTB" \
    --device="virtio-block:$IMG" --device="virtio-rng" \
    --max-cycles="$MAX_CYCLES" --loglevel=warn < "$FIFO" > "$LOG" 2>&1 &
EMU_PID=$!

wait_for() {
    local pat=$1 secs=$2 n=0
    while [ "$n" -lt $((secs * 2)) ]; do
        if grep -qF -- "$pat" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        n=$((n + 1))
    done
    return 1
}

send_cmd() {
    printf '%s\n' "$1" >&9
    sleep "${2:-2}"
}

# ---- 阶段 1：进入 busybox initramfs ----
if wait_for 'Please press Enter' 300; then
    echo "==> initramfs console 就绪"
else
    echo "==> warning: 没等到 initramfs 提示符" >&2
fi
sleep 1
printf '\n' >&9
if wait_for '~ #' 120; then
    echo "==> initramfs shell 就绪"
else
    echo "==> warning: 没等到 shell" >&2
fi
sleep 2

# ---- 阶段 2：交接给 Alpine rootfs ----
send_cmd 'echo ===PREP===' 1
send_cmd 'mount -t devtmpfs devtmpfs /dev; echo dev_rc=$?' 2
send_cmd 'mkdir -p /mnt && mount /dev/vda /mnt; echo root_rc=$?' 3
send_cmd 'cat /mnt/etc/alpine-release' 2
send_cmd 'ls /mnt | head -20' 2
send_cmd 'mkdir -p /mnt/dev /mnt/proc /mnt/sys' 2
send_cmd 'mount -t devtmpfs devtmpfs /mnt/dev; echo dev2_rc=$?' 2
send_cmd 'mount -t proc proc /mnt/proc; echo proc_rc=$?' 2
send_cmd 'mount -t sysfs sysfs /mnt/sys; echo sys_rc=$?' 2
send_cmd 'ls /mnt/dev/console' 2

# 这里本来想用 switch_root，但它会失败：
#   busybox 源码 util-linux/switch_root.c:
#     if (!dry_run && getpid() != 1) bb_show_usage();
#   我们的 shell 是 initramfs 里 busybox init(PID 1) 的子进程，所以被判非法。
# 先把证据打出来，再用 chroot 交接。
send_cmd 'echo ===PIDCHECK===' 1
send_cmd 'echo shell_pid=$$; echo -n "pid1_comm="; cat /proc/1/comm' 2
# 不再实跰 switch_root：它一次吐十几行 usage，UART 会把下一条命令丢掉。
# 它为什么失败见 doc/chapter6-linux.md §7.3。
send_cmd 'echo ===CHROOT===' 1
send_cmd "chroot /mnt /bin/sh -c 'cat /etc/alpine-release; uname -a; id; apk --version; cat /etc/apk/repositories; cat /proc/mounts; ls -l /dev/vd* /dev/hwrng; dd if=/dev/hwrng bs=8 count=1 2>/dev/null | od -An -tx1; head -2 /etc/os-release'" 12
send_cmd 'echo ===DONE2===' 1

send_cmd 'poweroff -f' 6

for _ in $(seq 1 40); do
    if ! kill -0 "$EMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done
if kill -0 "$EMU_PID" 2>/dev/null; then
    echo "==> 模拟器未自行退出，强制结束"
    kill "$EMU_PID" 2>/dev/null
fi
wait "$EMU_PID" 2>/dev/null
exec 9>&-
rm -f "$FIFO"

echo "==> 完成，日志：$LOG"
echo "=========== guest 输出 ==========="
sed -n '/===PREP===/,$p' "$LOG" | head -200
