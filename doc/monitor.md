# Chapter 4 实验报告：M-mode Monitor

## 1. 目标

在 HERE 模拟器上实现一个最小可用的 **M-mode 监控程序（monitor）**：

| 章节要求 | 实现方式 |
| --- | --- |
| 从 M-mode 进入 U-mode 运行任务，trap 后可靠回到 M-mode handler | `trap.S` 的 `mscratch` 换栈 + `mstatus.MPP=U` |
| 设计若干 `ecall` 服务 | 7 个系统调用（见 §3） |
| 安排一项 ecall 之外的异常，检查 mcause/mepc/mtval | 执行 `.word 0x00000000` 触发 IllegalInstruction |
| 接收 CLINT 定时器中断，公平分配时间片 | `mtimecmp += QUANTUM`，轮转调度 |

## 2. 特权级切换模型

```
        ┌──────────────── M-mode ────────────────┐
        │  trap_handler(ctx)                     │
        │    ├─ 定时器中断 → 抢占调度            │
        │    ├─ UserEnvCall → 系统调用           │
        │    └─ 其它异常 → 打印诊断 + 杀任务     │
        └───────────────▲───────────┬────────────┘
                  trap  │           │ __traps_return (mret)
                        │           ▼
        ┌───────────────┴──── U-mode ────────────┐
        │  任务 A / B / X / Y / sleeper / bad    │
        │  通过 ecall 请求服务，无 syscall 权限  │
        └────────────────────────────────────────┘
```

关键点：

- `medeleg = mideleg = 0`：**不委托**任何异常/中断，全部在 M-mode 处理。
- `trap.S` 用 `csrrw sp, mscratch, sp` 把用户栈换成内核栈，`__traps_return` 再换回去。
- `TrapContext` 必须是 `Task` 结构体的**最后一个成员**，这样 `sp += 34*8` 正好落在内核栈顶。

## 3. 系统调用表

`a7` = 服务号，`a0/a1` = 参数，返回值写回 `a0`。

| a7 | 名称 | 参数 | 说明 |
| --- | --- | --- | --- |
| 0 | `exit` | – | 结束当前任务；全部结束时 `PowerOff()` |
| 1 | `putchar` | `a0`=字符 | 输出单字符 |
| 2 | `print` | `a0`=字符串 | 输出字符串 |
| 3 | `yield` | – | 主动让出 CPU |
| 4 | `sleep` | `a0`=tick 数 | 睡眠直到 `tick_count >= 目标` |
| 5 | `spawn` | `a0`=入口, `a1`=用户栈顶 | 动态创建任务，返回 tid |
| 6 | `get_time` | – | 返回 `mtime` |

## 4. 调度器

- **任务状态**：`UNUSED / READY / SLEEPING / ZOMBIE`
- **时间片**：`QUANTUM_TICKS = 1024` tick（1 tick = 8 cycle，约 8192 cycle）
- **抢占源**：CLINT `mtime >= mtimecmp` → `mip.MTIP` → `mcause = 0x8000000000000007`
- **ack 方式**：在 handler 里写 `mtimecmp = mtime + QUANTUM`，这是唯一的正确清中断方式
- **轮转**：`pick_next()` 从 `current+1` 开始环形扫描 READY 任务，优先真实任务，兜底 idle

> 注意：HERE 在 `try_send_trap_signal` 中的判据是
> `level < PrivilegeLevel::M || mstatus.MIE == 1`，
> 因此 **U-mode 任务即使 `MIE=0` 也会收到定时器中断**，
> 抢占真正依赖的是 `mie.MTIE=1` + 有效的 `mtimecmp`。

## 5. 初始化顺序（踩过的坑）

定时器必须在**所有任务创建完成之后**才启用，否则中断会在 `main` 初始化途中触发，
handler 拿到 `current = -1`，此时 `pick_next()` 会调度出一个还没完整的任务集合。

正确顺序：

```c
csr_write(CSR_MEDELEG, 0);
csr_write(CSR_MIDELEG, 0);

trap_init();                                            /* 装 mtvec / mscratch / mie.MTIE */

csr_write(CSR_MSTATUS, csr_read(CSR_MSTATUS) & ~(1UL << 3));   /* 关 MIE */

task_create(idle_entry, ...);
task_create(task_a, ...);
/* ... 其余任务 ... */

printf("[monitor] starting with %d user tasks ...\n", alive_count);

write_mtimecmp(read_mtime() + QUANTUM_TICKS);           /* 现在才装定时器 */
csr_write(CSR_MSTATUS, csr_read(CSR_MSTATUS) | (1UL << 3));    /* 开 MIE */

switch_to(1);
```

另一个坑：`SYS_exit` 必须调用 `task_finish_current()`，
否则任务只让出 CPU 而不会被标记成 ZOMBIE，会一直被调度、在 `for(;;)` 里空转。

## 6. 运行结果

```
[monitor] starting with 7 user tasks, quantum=1024 ticks
[A][B]XXXXXXXXXXXX...YYYYYYYYYYYY...<S><BAD>
[monitor] task 6 killed: mcause=2 mepc=0x80001890 mtval=0x00000000
<SPAWNER><SPAWN-OK>
[monitor] task 7 exited
<SPAWNED>
[monitor] task 8 exited
[A][B]XXXX...YYYY...[A][B]XXXX...YYYY...
...
[monitor] task 1 exited
[monitor] task 2 exited
XXX...YYY...
[monitor] task 4 exited
XXXXX...
[monitor] task 3 exited
<S:wake>
[monitor] task 5 exited

[monitor] all user tasks finished

Used time: 0.371079s
```

### 现象解读

1. `[A][B][A][B]...` —— `yield` 服务正常，A/B 交替 12 轮后各自 `exit`。
2. `X`/`Y` 交替出现且各自成段 —— 两个任务**都不主动让出**，靠定时器抢占，
   每段长度 ≈ 一个时间片内 UART 能输出的字符数。
3. `<S>` → `<S:wake>` —— `sleep(2000)` 在两个时间片后准时唤醒。
4. `<BAD>` 后打印 `mcause=2 mepc=0x80001890 mtval=0x00000000`：
   - `mcause = 2` → IllegalInstruction；
   - `mtval = 0` 正是那条 `0x00000000` 非法指令本身；
   - 任务被杀死，**系统继续运行**，其余任务不受影响。
5. `<SPAWNER><SPAWN-OK>` → `<SPAWNED>` —— `spawn` + `get_time` 正常，
   新任务被插入轮转队列并正常退出。
6. 全部任务退出后 `PowerOff()`，模拟器干净停机。

## 7. 构建与运行

```bash
make -C ./test_resources bin/monitor.elf
./target/fast-debug/here ./test_resources/bin/monitor.elf --max-cycles 200000000
```

## 8. 结论

章节的四项要求全部达成：

- ✅ M-mode ↔ U-mode 的双向切换稳定，`trap.S` 的换栈逻辑在多次 preempt 下没有泄漏。
- ✅ 7 个 ecall 服务实现并实测（`putchar/print/yield/sleep/spawn/exit/get_time`）。
- ✅ U-mode 非法指令被捕获，`mcause/mepc/mtval` 与预期一致，故障隔离有效。
- ✅ CLINT 定时器驱动的时间片轮转让死循环任务公平共享 CPU。
