#!/bin/bash
# Chapter 6 实践 1：启动 HERE 模拟器里的 Linux，挂载 VirtIO-Block，运行 user_app。
#
# 为什么用 FIFO + 轮询日志，而不是直接 pipe 一串命令：
#
#   模拟器的 UART 输入链路有两个特性会让「定时灌入」失败：
#     1. `update_rx_status()` 每次只从有界 channel 取 1 字节（要等 guest 读 RBR）；
#     2. `write()` 里有一句 `while input_rx.try_recv().is_ok() {}`，
#        guest 的 tty 驱动打开/复位串口时会把排队中的输入全部丢弃。
#
#   所以固定 sleep 之后一次性喂命令，很可能整批被丢。
#   这里改成：把 stdin 接到一个 FIFO，轮询日志直到看见 shell 提示符，再逐条发送。
#
# 另外两个坑：
#   - initramfs 的 /dev 不是 devtmpfs，必须 `mount -t devtmpfs devtmpfs /dev`；
#   - 自动化必须给 --max-cycles 兜底，否则 guest 不响应时模拟器永不退出。
set -u

HERE_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=$HERE_DIR/linux/out
KERNEL=${KERNEL:-/mnt/c/Users/veyro/Downloads/linux-busybox-rv64gc.bin}
DTB=${DTB:-$HERE_DIR/dts/virt.dtb}
IMG=${IMG:-$OUT_DIR/block_image}
EMU=${EMU:-$HERE_DIR/target/release/here}
LOG=${LOG:-$OUT_DIR/boot.log}
MAX_CYCLES=${MAX_CYCLES:-20000000000}
FIFO=${FIFO:-/tmp/here_uart.fifo}

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

echo "==> kernel     : $KERNEL"
echo "==> dtb        : $DTB"
echo "==> block image: $IMG"
echo "==> emulator   : $EMU"
echo "==> log        : $LOG"

# 模拟器从 FIFO 读 stdin。
# 注意必须用 `9<>`（读写）打开：如果只开写端 `9>`，open 会阻塞到有读端出现，
# 而读端（模拟器）在下一步才启动，双方互等 -> 死锁。`<>` 不会阻塞。
exec 9<> "$FIFO"
"$EMU" "$KERNEL" --dtb="$DTB" \
    --device="virtio-block:$IMG" --device="virtio-rng" \
    --max-cycles="$MAX_CYCLES" --loglevel=warn < "$FIFO" > "$LOG" 2>&1 &
EMU_PID=$!

# 等日志里出现某个模式（每 0.5s 检查一次）
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

# 发一条命令并等待一会儿，让 guest 消费
send_cmd() {
    printf '%s\n' "$1" >&9
    sleep "${2:-2}"
}

if wait_for 'Please press Enter' 300; then
    echo "==> console 已就绪"
else
    echo "==> warning: 没等到 console 提示符" >&2
fi
sleep 1

# 激活 console（busybox init 要一个回车）
printf '\n' >&9
if wait_for '~ #' 120; then
    echo "==> shell 已就绪"
else
    echo "==> warning: 没等到 shell 提示符" >&2
fi
sleep 2

send_cmd 'echo ===DEV===' 1
send_cmd 'mount -t devtmpfs devtmpfs /dev; echo devtmpfs_rc=$?' 2
send_cmd 'ls /dev/vd* 2>&1' 2
send_cmd 'echo ===BLK===' 1
send_cmd 'dmesg | grep -i -e virtio_blk -e virtio_rng' 3
send_cmd 'echo ===MOUNT===' 1
send_cmd 'mkdir -p /mnt; mount /dev/vda /mnt; echo mount_rc=$?' 3
send_cmd 'ls -l /mnt' 2
send_cmd 'echo ===APP===' 1
send_cmd '/mnt/user_app chapter6-is-running' 4
send_cmd 'echo ===NOTE===' 1
send_cmd 'cat /mnt/host_note.txt' 2
send_cmd 'echo ===HWRNG===' 1
send_cmd 'od -An -tx1 -N 32 /dev/hwrng' 3
send_cmd 'echo ===VIRTIO===' 1
send_cmd 'for d in /sys/bus/virtio/devices/virtio*; do echo -n "$d -> "; readlink $d/driver; done' 3
send_cmd 'echo ===DONE===' 1

printf 'poweroff -f\n' >&9
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
