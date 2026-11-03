// hanoi.c —— 汉诺塔单步可视化演示（HERE 裸机移植）
//
// 设计目标：
//   1. 完全不依赖操作系统：不用 malloc、不用系统调用、不用 libc
//   2. 终端呈现：通过 UART 输出 ANSI 转义序列，实现清屏与彩色圆盘
//   3. 单步可视化：每一次移动后重绘棋盘，并用 CLINT mtime 做延时
//   4. 可验证：输出是确定性的，最后打印移动次数与理论值对比
//
// 裸机要点：
//   - 输出设备：UART 16550A，MMIO 基址 0x10000000（见 lib/io.c）
//   - 定时器：  CLINT mtime，基址 0x0200_0000 + 0xbff8
//               HERE 的虚拟时钟为 `cycles >> 3`，因此延时以周期计
//   - 停机：    写 power-manager（0x0010_0000）= 0x5555（见 lib/power.c）

#include "io.h"
#include "power.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 硬件地址与可调参数                                                   */
/* ------------------------------------------------------------------ */

#define CLINT_BASE        0x2000000ULL
#define MTIME_OFFSET      0xbff8ULL

#define NUM_DISKS         5                        /* 圆盘数量（2^N - 1 次移动） */
#define CELL_WIDTH        (2 * NUM_DISKS + 1)      /* 每根柱子占用的字符宽度 */
#define STEP_DELAY_TICKS  100000ULL                /* 每步延时（mtime tick，1 tick = 8 周期） */
#define COLORS            6

/* ------------------------------------------------------------------ */
/* 全局状态                                                             */
/* ------------------------------------------------------------------ */

/* pegs[p][0] 是最底下的圆盘；数值越大表示圆盘越大 */
static int pegs[3][NUM_DISKS];
static int heights[3];

static unsigned move_count;
static int last_from = -1;
static int last_to = -1;

/* ------------------------------------------------------------------ */
/* 硬件访问                                                             */
/* ------------------------------------------------------------------ */

static uint64_t read_mtime(void) {
    return *(volatile uint64_t *)(CLINT_BASE + MTIME_OFFSET);
}

static void delay_ticks(uint64_t ticks) {
    uint64_t start = read_mtime();
    while (read_mtime() - start < ticks) {
        /* 忙等待；HERE 的 mtime 由虚拟时钟驱动 */
    }
}

/* ------------------------------------------------------------------ */
/* 终端绘制辅助                                                         */
/* ------------------------------------------------------------------ */

static void put_spaces(int n) {
    while (n-- > 0) {
        uart_putc(' ');
    }
}

static void put_repeat(char c, int n) {
    while (n-- > 0) {
        uart_putc(c);
    }
}

/*
 * 绘制一个格子：
 *   size == 0 -> 画柱子 "|"
 *   size >  0 -> 画宽度为 (2*size-1) 的彩色圆盘
 *
 * 每个格子固定宽度 CELL_WIDTH，保证画面稳定不抖动。
 */
static void draw_cell(int size) {
    int width = (size == 0) ? 1 : (2 * size - 1);
    int pad   = (CELL_WIDTH - width) / 2;

    put_spaces(pad);

    if (size == 0) {
        uart_putc('|');
    } else {
        /* ANSI 前景色：31..36 */
        int color = 31 + ((size - 1) % COLORS);
        printf("\x1b[%dm", color);
        put_repeat('=', width);
        printf("\x1b[0m");
    }

    put_spaces(CELL_WIDTH - pad - width);
}

static void draw_board(void) {
    /* 清屏并把光标移到左上角 */
    printf("\x1b[2J\x1b[H");

    printf("==== Tower of Hanoi ====\n");
    printf("disks: %d    moves: %d", NUM_DISKS, (int)move_count);

    if (last_from >= 0) {
        printf("    last: %c -> %c", 'A' + last_from, 'A' + last_to);
    }
    printf("\n\n");

    /* 从上往下逐层绘制 */
    for (int level = NUM_DISKS - 1; level >= 0; level--) {
        for (int p = 0; p < 3; p++) {
            int size = (level < heights[p]) ? pegs[p][level] : 0;
            draw_cell(size);
            put_spaces(2);
        }
        uart_putc('\n');
    }

    /* 底座 */
    for (int p = 0; p < 3; p++) {
        put_repeat('-', CELL_WIDTH);
        put_spaces(2);
    }
    uart_putc('\n');

    /* 柱子标签 A / B / C */
    for (int p = 0; p < 3; p++) {
        int pad = (CELL_WIDTH - 1) / 2;
        put_spaces(pad);
        uart_putc('A' + p);
        put_spaces(CELL_WIDTH - pad - 1);
        put_spaces(2);
    }
    uart_putc('\n');
}

/* ------------------------------------------------------------------ */
/* 汉诺塔逻辑                                                           */
/* ------------------------------------------------------------------ */

static void move_disk(int from, int to) {
    int disk = pegs[from][--heights[from]];
    pegs[to][heights[to]++] = disk;

    move_count++;
    last_from = from;
    last_to   = to;
}

static void hanoi(int n, int from, int to, int via) {
    if (n == 0) {
        return;
    }

    hanoi(n - 1, from, via, to);

    move_disk(from, to);
    draw_board();
    delay_ticks(STEP_DELAY_TICKS);

    hanoi(n - 1, via, to, from);
}

/* ------------------------------------------------------------------ */
/* 入口                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    /* 初始化：所有圆盘按从大到小堆在 A 柱（pegs[0]） */
    for (int i = 0; i < NUM_DISKS; i++) {
        pegs[0][i] = i + 1;
    }
    heights[0] = NUM_DISKS;
    heights[1] = 0;
    heights[2] = 0;

    move_count = 0;

    draw_board();
    delay_ticks(STEP_DELAY_TICKS);

    hanoi(NUM_DISKS, 0, 2, 1);

    draw_board();
    printf("\nDone! %d moves (theoretical %d)\n",
           (int)move_count, (1 << NUM_DISKS) - 1);

    /* 裸机退出方式：请求模拟器停机 */
    PowerOff();

    return 0;
}
