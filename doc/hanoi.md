# 裸机程序移植实验：汉诺塔单步可视化

本实验把一个"终端可视化程序"移植到 HERE 模拟器的裸机环境，不依赖任何操作系统服务。

- 源码：`test_resources/src/hanoi.c`
- 构建目标：`hanoi`（已加入 `test_resources/Makefile` 的 `TARGETS`）
- 产物：`test_resources/bin/hanoi.elf`

---

## 1. 选型理由

候选方向与取舍：

| 方向 | 可行性 | 说明 |
|---|---|---|
| 交互式小游戏（如贪吃蛇） | 中 | 需要非阻塞输入 + 实时计时，且难以自动化验证 |
| 简易计算器 | 高 | 逻辑简单，但终端呈现效果较弱 |
| Shell 命令解释器 | 中 | 需要字符串解析与命令表，展示性一般 |
| **单步算法可视化** | **高** | **无需输入、输出确定、能展示完整的裸机能力栈** |

最终选择 **汉诺塔（Tower of Hanoi）可视化**，因为它同时覆盖：

- 递归算法（栈的使用）
- 终端渲染（ANSI 转义序列、彩色输出）
- 硬件定时器（CLINT mtime 延时）
- MMIO 输出（UART）
- 确定的、可验证的输出

---

## 2. 裸机约束与实现对照

| 约束 | 实现方式 |
|---|---|
| 没有 `malloc` | 全部状态使用静态数组：`static int pegs[3][NUM_DISKS]` |
| 没有 `printf` | 使用 `test_resources/lib/io.c` 中手写的 `printf` / `uart_putc` |
| 没有系统调用 | 直接读写 MMIO |
| 没有 `sleep()` | 忙等待 CLINT `mtime` |
| 没有 `exit()` | 向 power-manager 写 `0x5555` |
| 没有终端控制库 | 手写 ANSI 转义序列 |

### 硬件地址

| 作用 | 地址 | 说明 |
|---|---|---|
| UART 数据寄存器 | `0x10000000` | 见 `lib/io.c` |
| UART 状态寄存器 | `0x10000005` | bit5 = 发送缓冲区空 |
| CLINT mtime | `0x02000000 + 0xbff8` | 虚拟时钟为 `cycles >> 3` |
| power-manager | `0x00100000` | 写 `0x5555` 触发停机 |

---

## 3. 实现要点

### 3.1 数据结构

```c
static int pegs[3][NUM_DISKS];   /* pegs[p][0] 为最底层圆盘 */
static int heights[3];           /* 每根柱子当前高度 */
static unsigned move_count;      /* 已移动次数 */
```

### 3.2 延时

HERE 的 CLINT `mtime` 来自虚拟时钟：

```rust
// src/clock.rs
fn now(&self) -> u64 { self.cycles.get() >> 3 }
```

所以延时按**周期**计算，而不是墙钟时间：

```c
#define CLINT_BASE      0x2000000ULL
#define MTIME_OFFSET    0xbff8ULL

static uint64_t read_mtime(void) {
    return *(volatile uint64_t *)(CLINT_BASE + MTIME_OFFSET);
}

static void delay_ticks(uint64_t ticks) {
    uint64_t start = read_mtime();
    while (read_mtime() - start < ticks) { }
}
```

### 3.3 终端渲染

每一帧先清屏再重绘：

```c
printf("\x1b[2J\x1b[H");
```

彩色圆盘使用 ANSI 前景色：

```c
int color = 31 + ((size - 1) % 6);   /* 31..36 */
printf("\x1b[%dm", color);
/* 输出 '=' 组成的圆盘 */
printf("\x1b[0m");
```

### 3.4 算法

```c
static void hanoi(int n, int from, int to, int via) {
    if (n == 0) return;
    hanoi(n - 1, from, via, to);
    move_disk(from, to);
    draw_board();
    delay_ticks(STEP_DELAY_TICKS);
    hanoi(n - 1, via, to, from);
}
```

---

## 4. 构建与运行

```bash
export PATH=/opt/riscv/bin:$PATH

make -C ./test_resources bin/hanoi.elf

cargo run --profile fast-debug -- ./test_resources/bin/hanoi.elf
```

在真实终端中运行时，可以看到彩色圆盘的逐帧动画。

> 如果是在非终端环境（输出重定向、CI、脚本）中运行，建议加 `--max-cycles`
> 以确保正常停机并刷新输出：
>
> ```bash
> ./target/fast-debug/here ./test_resources/bin/hanoi.elf --max-cycles 60000000
> ```

---

## 5. 验证结果

使用 `--max-cycles 60000000` 运行的实测结果：

| 指标 | 结果 |
|---|---|
| 输出字节数 | 13,889 |
| 画面帧数（`Tower of Hanoi` 出现次数） | **33** = 初始 1 + 31 次移动 + 最终 1 |
| 结束标志 | `Done! 31 moves (theoretical 31)` ✅ |
| 模拟耗时 | 约 0.65 秒 |

最终画面（去掉 ANSI 转义后）：

```
==== Tower of Hanoi ====
disks: 5    moves: 31    last: A -> C

     |            |        =========
     |            |         =======
     |            |          =====
     |            |           ===
     |            |            =
-----------  -----------  -----------
     A            B            C

Done! 31 moves (theoretical 31)
```

所有圆盘按"大在下、小在上"的顺序落在 C 柱，移动次数 31 = `2^5 - 1`，算法与渲染均正确。

---

## 6. 可调参数

位于 `hanoi.c` 顶部：

| 宏 | 默认值 | 作用 |
|---|---|---|
| `NUM_DISKS` | `5` | 圆盘数量（移动次数为 `2^N - 1`） |
| `STEP_DELAY_TICKS` | `100000` | 每步延时（1 tick = 8 模拟周期） |
| `COLORS` | `6` | 可选 ANSI 颜色数量 |

调整示例：

```c
#define NUM_DISKS        4
#define STEP_DELAY_TICKS 200000ULL
```

---

## 7. 延伸实验

1. **改成交互式**：每步等待一个按键。需要直接轮询 UART 的 LSR bit0（RDR），而不是调用阻塞的 `uart_getc()`。
2. **加暂停/加速**：读取按键动态调整 `STEP_DELAY_TICKS`。
3. **改成其他算法可视化**：冒泡排序、快速排序、迷宫求解，渲染框架可直接复用。
4. **统计信息**：记录每根柱子的移动次数、最大递归深度等。

---

## 8. 小结

这个实验验证了裸机程序所需的完整能力栈：

```
链接脚本定位 → 启动汇编设栈 → C 运行时 → MMIO 输出
            → 定时器延时 → 算法逻辑 → 设备停机
```

并且全程只使用了 HERE 提供的硬件接口，没有任何操作系统服务。
