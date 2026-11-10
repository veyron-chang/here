// monitor.c —— Chapter 4：M-mode monitor + U-mode 多任务 + 定时器抢占
//
// 结构：
//   U-mode 任务  ── ecall ──>  M-mode trap_handler
//                                   ├─ syscall 分发
//                                   ├─ 异常诊断 / 杀任务
//                                   └─ 定时器中断 → 轮转调度
//
// 复用：
//   lib/trap.S          __traps_entry / __traps_return（mscratch 换栈）
//   lib/trap_handler.c  trap_init()（设置 mtvec/mie/mstatus）
//   lib/io.c            printf / uart_putc
//   lib/power.c         PowerOff()

#include "io.h"
#include "power.h"
#include "trap.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 可调参数                                                             */
/* ------------------------------------------------------------------ */

#define MAX_TASKS        10
#define KSTACK_SIZE      8192          /* 每个任务的内核栈 */
#define USTACK_SIZE      4096          /* 每个任务的用户栈 */
#define QUANTUM_TICKS    1024          /* 时间片（mtime tick） */

#define CLINT_BASE       0x2000000UL
#define MTIME_OFFSET     0xbff8UL
#define MTIMECMP_OFFSET  0x4000UL

#define CSR_MEDELEG      0x302UL
#define CSR_MIDELEG      0x303UL

/* ------------------------------------------------------------------ */
/* 任务状态                                                             */
/* ------------------------------------------------------------------ */

enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_SLEEPING,
    TASK_ZOMBIE,
};

typedef struct {
    uint8_t     kstack[KSTACK_SIZE];
    TrapContext ctx;                   /* 必须是最后一个成员：ctx 必须贴着内核栈顶 */
} Task;

static Task     tasks[MAX_TASKS];
static uint8_t  ustacks[MAX_TASKS][USTACK_SIZE] __attribute__((aligned(16)));

static int      task_state[MAX_TASKS];
static uint64_t task_wake[MAX_TASKS];  /* sleep 唤醒的 tick 数 */
static int      task_alive[MAX_TASKS]; /* 是否计入 alive_count */

static int      current = -1;
static uint64_t tick_count = 0;
static int      alive_count = 0;
static int      need_resched = 0;

#define IDLE_TASK 0

/* ------------------------------------------------------------------ */
/* 基础工具                                                             */
/* ------------------------------------------------------------------ */

/*
 * CSR 访问必须使用编译期立即数：
 * csrr/csrw 的 CSR 编号不能来自寄存器。
 * 因此这里用宏而不是函数，保证 addr 在调用点展开成字面量。
 */
#define csr_read(addr)                                                       \
    ({                                                                       \
        uint64_t __csr_v;                                                    \
        asm volatile("csrr %0, %1" : "=r"(__csr_v) : "i"(addr));             \
        __csr_v;                                                             \
    })

#define csr_write(addr, val)                                                 \
    asm volatile("csrw %0, %1" : : "i"(addr), "r"((uint64_t)(val)))

static inline uint64_t read_mtime(void) {
    return *(volatile uint64_t *)(CLINT_BASE + MTIME_OFFSET);
}

static inline void write_mtimecmp(uint64_t v) {
    *(volatile uint64_t *)(CLINT_BASE + MTIMECMP_OFFSET) = v;
}

static void zero_bytes(void *p, unsigned n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) {
        *b++ = 0;
    }
}

static uint64_t ustack_top(int i) {
    return (uint64_t)(ustacks[i] + USTACK_SIZE);
}

/* ------------------------------------------------------------------ */
/* 任务管理                                                             */
/* ------------------------------------------------------------------ */

static int task_create(void (*entry)(void), uint64_t ustack_top_value, int count_alive) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_state[i] == TASK_UNUSED) {
            Task *t = &tasks[i];
            zero_bytes(&t->ctx, sizeof(TrapContext));

            t->ctx.x[2]    = ustack_top_value;   /* 用户 sp */
            t->ctx.mepc    = (uint64_t)entry;
            t->ctx.mstatus = (1UL << 7);         /* MPIE=1, MPP=U(0) */

            task_state[i] = TASK_READY;
            task_wake[i]  = 0;
            task_alive[i] = count_alive;

            if (count_alive) {
                alive_count++;
            }
            return i;
        }
    }
    return -1;
}

__attribute__((noreturn))
static void switch_to(int idx) {
    current = idx;
    __traps_return(&tasks[idx].ctx);
    __builtin_unreachable();
}

/// 优先选择真实任务；没有就回到 idle。
static int pick_next(void) {
    int start = (current >= 0) ? current : 0;

    for (int i = 1; i <= MAX_TASKS; i++) {
        int j = (start + i) % MAX_TASKS;
        if (j != IDLE_TASK && task_state[j] == TASK_READY) {
            return j;
        }
    }
    return IDLE_TASK;
}

static void task_finish_current(void) {
    if (current >= 0 && task_alive[current]) {
        task_alive[current] = 0;
        task_state[current] = TASK_ZOMBIE;
        alive_count--;
    }

    if (alive_count == 0) {
        printf("\n[monitor] all user tasks finished\n");
        PowerOff();
    }
}

/* ------------------------------------------------------------------ */
/* U-mode 系统调用封装                                                  */
/* ------------------------------------------------------------------ */

static inline uint64_t sys0(uint64_t nr) {
    register uint64_t a0 asm("a0") = 0;
    register uint64_t a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline uint64_t sys1(uint64_t nr, uint64_t x) {
    register uint64_t a0 asm("a0") = x;
    register uint64_t a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline uint64_t sys2(uint64_t nr, uint64_t x, uint64_t y) {
    register uint64_t a0 asm("a0") = x;
    register uint64_t a1 asm("a1") = y;
    register uint64_t a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

#define SYS_exit     0
#define SYS_putchar  1
#define SYS_print    2
#define SYS_yield    3
#define SYS_sleep    4
#define SYS_spawn    5
#define SYS_get_time 6

static inline long sys_exit(long code)               { return (long)sys1(SYS_exit, (uint64_t)code); }
static inline long sys_putchar(char c)               { return (long)sys1(SYS_putchar, (uint64_t)(unsigned char)c); }
static inline long sys_print(const char *s)          { return (long)sys1(SYS_print, (uint64_t)s); }
static inline long sys_yield(void)                   { return (long)sys0(SYS_yield); }
static inline long sys_sleep(uint64_t ticks)         { return (long)sys1(SYS_sleep, ticks); }
static inline long sys_spawn(void (*entry)(void), uint64_t sp) { return (long)sys2(SYS_spawn, (uint64_t)entry, sp); }
static inline uint64_t sys_get_time(void)            { return sys0(SYS_get_time); }

/* ------------------------------------------------------------------ */
/* U-mode 测试任务                                                      */
/* ------------------------------------------------------------------ */

static void idle_entry(void) {
    for (;;) {
        asm volatile("nop");
    }
}

static void task_a(void) {
    for (int i = 0; i < 12; i++) {
        sys_print("[A]");
        sys_yield();
    }
    sys_exit(0);
    for (;;) { }
}

static void task_b(void) {
    for (int i = 0; i < 12; i++) {
        sys_print("[B]");
        sys_yield();
    }
    sys_exit(0);
    for (;;) { }
}

/* 纯计算 + 输出，不主动 yield：用来验证定时器抢占 */
static void task_spin_x(void) {
    for (int i = 0; i < 600; i++) {
        sys_putchar('X');
    }
    sys_exit(0);
    for (;;) { }
}

static void task_spin_y(void) {
    for (int i = 0; i < 600; i++) {
        sys_putchar('Y');
    }
    sys_exit(0);
    for (;;) { }
}

static void task_sleeper(void) {
    sys_print("<S>");
    sys_sleep(2000);
    sys_print("<S:wake>");
    sys_exit(0);
    for (;;) { }
}

/* 故意触发非法指令，验证 mcause/mepc/mtval */
static void task_bad(void) {
    sys_print("<BAD>");
    asm volatile(".word 0x00000000");   /* 非法指令 */
    sys_exit(0);
    for (;;) { }
}

/* 动态创建的任务：验证 spawn 服务 */
static void task_spawned(void) {
    sys_print("<SPAWNED>");
    sys_exit(0);
    for (;;) { }
}

/* 调用 get_time + spawn */
static void task_spawner(void) {
    sys_print("<SPAWNER>");

    uint64_t t = sys_get_time();
    long tid = sys_spawn(task_spawned, ustack_top(9));

    if (tid >= 0 && t != 0) {
        sys_print("<SPAWN-OK>");
    } else {
        sys_print("<SPAWN-FAIL>");
    }

    sys_exit(0);
    for (;;) { }
}

/* ------------------------------------------------------------------ */
/* M-mode trap 处理                                                     */
/* ------------------------------------------------------------------ */

static void on_timer_tick(void) {
    /* 写 mtimecmp 是唯一正确的“应答”方式：会清掉 MTIP */
    write_mtimecmp(read_mtime() + QUANTUM_TICKS);
    tick_count++;

    /* 唤醒到期的 sleep 任务 */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_state[i] == TASK_SLEEPING && tick_count >= task_wake[i]) {
            task_state[i] = TASK_READY;
        }
    }

    need_resched = 1;
}

static void syscall_dispatch(TrapContext *ctx) {
    uint64_t nr = ctx->x[17];   /* a7 */
    uint64_t a0 = ctx->x[10];
    uint64_t a1 = ctx->x[11];

    ctx->mepc += 4;             /* 跳过 ecall 指令 */
    ctx->x[10] = 0;             /* 默认返回值 */

    switch (nr) {
    case SYS_exit:
        printf("\n[monitor] task %d exited\n", current);
        task_finish_current();
        need_resched = 1;
        break;

    case SYS_putchar:
        uart_putc((char)a0);
        break;

    case SYS_print: {
        const char *s = (const char *)a0;
        long n = 0;
        while (*s) {
            uart_putc(*s++);
            n++;
        }
        ctx->x[10] = (uint64_t)n;
        break;
    }

    case SYS_yield:
        need_resched = 1;
        break;

    case SYS_sleep:
        task_state[current] = TASK_SLEEPING;
        task_wake[current]  = tick_count + a0;
        need_resched = 1;
        break;

    case SYS_spawn:
        ctx->x[10] = (uint64_t)task_create((void (*)(void))a0, a1, 1);
        break;

    case SYS_get_time:
        ctx->x[10] = read_mtime();
        break;

    default:
        printf("\n[monitor] unknown syscall %d\n", (int)nr);
        ctx->x[10] = (uint64_t)-1;
        break;
    }
}

void trap_handler(TrapContext *ctx) {
    uint64_t mcause = csr_read(CSR_MCAUSE);
    int is_interrupt = (int)(mcause >> 63);
    uint64_t code = mcause & ((1UL << 63) - 1);

    if (is_interrupt) {
        if (code == 7) {                 /* Machine Timer */
            on_timer_tick();
        }
        /* 其他中断本轮忽略 */
    } else if (code == 8) {              /* UserEnvCall */
        syscall_dispatch(ctx);
    } else {
        /* 非 ecall 异常：打印现场并结束该任务 */
        printf("\n[monitor] task %d killed: mcause=%d mepc=%08x mtval=%08x\n",
               current,
               (int)code,
               (unsigned)csr_read(CSR_MEPC),
               (unsigned)csr_read(CSR_MTVAL));
        task_finish_current();
        need_resched = 1;
    }

    if (need_resched) {
        need_resched = 0;
        switch_to(pick_next());
    } else {
        switch_to(current);             /* 处理完继续跑当前任务 */
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    TEST_START(__BASE_FILE__);

    /* 关闭委托：让 U-mode 的 ecall 一定进入 M-mode */
    csr_write(CSR_MEDELEG, 0);
    csr_write(CSR_MIDELEG, 0);

    trap_init();

    /* 初始化期间关闭 M 态全局中断：避免定时器在任务创建完成前触发 */
    csr_write(CSR_MSTATUS, csr_read(CSR_MSTATUS) & ~(1UL << 3));

    /* idle 任务（index 0），不计入 alive_count */
    task_create(idle_entry, ustack_top(IDLE_TASK), 0);

    task_create(task_a,       ustack_top(1), 1);
    task_create(task_b,       ustack_top(2), 1);
    task_create(task_spin_x,  ustack_top(3), 1);
    task_create(task_spin_y,  ustack_top(4), 1);
    task_create(task_sleeper, ustack_top(5), 1);
    task_create(task_bad,     ustack_top(6), 1);
    task_create(task_spawner, ustack_top(7), 1);

    printf("[monitor] starting with %d user tasks, quantum=%d ticks\n",
           alive_count, QUANTUM_TICKS);

    /* 所有任务就绪后，才装定时器并重新打开中断 */
    write_mtimecmp(read_mtime() + QUANTUM_TICKS);
    csr_write(CSR_MSTATUS, csr_read(CSR_MSTATUS) | (1UL << 3));

    switch_to(1);

    /* 不会到达：所有任务结束后由 handler 调用 PowerOff() */
    for (;;) { }
}
