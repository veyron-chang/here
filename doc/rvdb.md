# rvdb 调试器命令手册

`rvdb` 是 HERE 内置的 RISC-V 调试器，用于观察和控制运行中的来宾程序。

- 命令定义源码：`src/rvdb/mod.rs`
- 命令实现源码：`src/rvdb/commands.rs`
- 调试器核心：`src/isa/riscv/debugger.rs`

---

## 1. 启动方式

### 交互式

```bash
cargo run --profile fast-debug -- <ELF 文件> -g
```

进入后显示 `(rvdb)` 提示符。

### 脚本模式

先用脚本执行一批命令，再进入交互：

```bash
cargo run --profile fast-debug -- <ELF> -g -S ./script.rvdb
```

`-S` 依赖 `-g`（`--script` 的 clap 约束 `requires = "debug"`）。

### 非交互式（管道 / 重定向）

当 stdin 或 stdout 不是终端时，rvdb 强制要求 `-S`：

```bash
cargo run --profile fast-debug -- <ELF> -g -S ./script.rvdb < /dev/null | cat
```

否则报错：

```
rvdb requires --script when stdin or stdout is not a terminal
```

---

## 2. 命令总览

| 分类 | 命令 | 别名 | 作用 |
|---|---|---|---|
| 观察 | `print` | `p` | 读取寄存器、内存、CSR |
| 观察 | `display` | `d` | 每次停机自动打印某项 |
| 观察 | `undisplay` | — | 取消自动打印 |
| 观察 | `translate` | `t`, `trans` | 虚拟地址翻译为物理地址 |
| 观察 | `list` | `l`, `ls` | 反汇编当前 PC 附近 |
| 观察 | `history` | `his` | 最近执行过的指令 |
| 执行 | `step` | `s`, `si` | 执行一条指令 |
| 执行 | `continue` | `c` | 继续执行，可限定步数 |
| 断点 | `break` | `b` | 设置或删除断点 |
| 追踪 | `ftrace` | `ft`, `f-trace` | 函数调用追踪 |
| 其他 | `symbol-file` | `symbol`, `file` | 加载 ELF 符号表 |
| 其他 | `info` | — | 查看断点或符号 |
| 其他 | `quit` | `q`, `exit` | 退出模拟器 |

输入 `help` 查看命令列表，`<命令> --help` 查看具体用法。

---

## 3. `print` / `p`

语法：`p <子命令> [参数]`

| 子命令 | 语法 | 说明 | 默认值 |
|---|---|---|---|
| `pc` | `p pc` | 当前程序计数器 | — |
| `reg` | `p reg <名称>` | 单个整数寄存器 | — |
| `regs` | `p regs [--start N] [-l N]` | 寄存器范围 | `start=0`，`len=32` |
| `mem` | `p mem <地址> [-l N] [-v]` | 读取内存字节 | `len=16`，物理地址 |
| `csr` | `p csr <名称\|地址>` | 读取 CSR | — |
| `freg` | `p freg <名称>` | 浮点寄存器 | — |
| `vreg` | `p vreg <名称>` | 向量寄存器 | — |
| `priv` | `p priv` | 当前特权级 | — |

### 示例

```rvdb
p pc
p reg sp
p reg a0
p regs
p regs --start 8 -l 10
p mem 0x80000000 -l 32
p mem 0x1000 -l 16 -v
p csr mstatus
p csr 0x300
p freg fa0
p vreg v0
p priv
```

### 注意

- `p mem` **默认按物理地址解释**，加 `-v` / `--virt` 才按虚拟地址读取。
- `p csr` 既接受名称（`mstatus`），也接受地址（`0x300`）。
- `p vreg` 会按多种元素宽度展示同一份向量数据。

---

## 4. `display` / `d` 与 `undisplay`

语法与 `print` 完全一致，区别是**每次来宾停机后自动打印**。

```rvdb
d pc
d regs
d csr mstatus
```

取消：

```rvdb
undisplay pc
undisplay regs
```

适合配合 `step` / `continue` 跟踪循环状态。

---

## 5. `step` / `s` / `si`

```rvdb
step
```

执行**一条**指令后返回提示符。

---

## 6. `continue` / `c`

```rvdb
continue        # 一直运行，直到断点或停机
c 1000          # 最多执行 1000 步
```

参数默认值为 `u64::MAX`（即不限制）。

> 内部实现：每次批量执行 `Board::STEP_BATCH_CYCLES = 1024` 个周期，
> 然后由 `ExecutionHook::after_step` 检查断点等停止条件。

---

## 7. `break` / `b`

```rvdb
break main                # 按符号名
break 0x80000100          # 按物理地址
break 0x1000 -v           # 按虚拟地址
break -d main             # 删除断点
break --delete 0x80000100
```

| 选项 | 说明 |
|---|---|
| `-v`, `--virt` | 把目标当作虚拟地址 |
| `-d`, `--delete` | 删除断点而不是设置 |

### 实现说明

断点**不会修改来宾内存**（不是插入 `ebreak`）。
调试器在执行每条指令后比较 PC：

- 虚拟断点：直接与当前 PC 比较
- 物理断点：先把当前 PC 翻译成物理地址，再比较

因此断点可以设在只读代码段上，也不会干扰程序行为。

---

## 8. `list` / `l` / `ls`

```rvdb
list
```

从当前 PC 开始反汇编。

与 `history` 的区别：

| 命令 | 观察对象 |
|---|---|
| `list` | **将要执行**的指令 |
| `history` | **已经执行过**的指令 |

---

## 9. `history` / `his`

```rvdb
history         # 默认最近 20 条
history 100
```

底层是容量为 1024 的环形缓冲（`MAX_HISTORY = 1024`），超出后丢弃最旧记录。

---

## 10. `ftrace` / `ft`

```rvdb
ftrace start        # 开始记录
ftrace stop         # 停止记录
ftrace show 50      # 显示最近 50 条调用/返回（默认 20）
ftrace stat         # 统计每个函数的调用/返回次数
```

### 统计字段

| 字段 | 含义 |
|---|---|
| `calls` | 该函数被调用次数 |
| `returns` | 该函数返回次数 |
| `unknown_calls` | 目标地址无法解析符号的调用数 |
| `unknown_returns` | 无法解析符号的返回数 |

### 实现原理

1. 识别跳转类指令：`jal`、`jalr`、`c.jal`、`c.jalr`
2. 判断目标寄存器是否为链接寄存器（`x1` / `x5`）
3. 据此推断 Push（调用）/ Pop（返回）/ PopThenPush（尾调用）
4. 用 `.symtab` 把地址映射为函数名

若未加载符号表，只能显示地址。

---

## 11. `translate` / `t` / `trans`

```rvdb
translate 0x1000          # 默认按读权限检查
translate 0x1000 write    # 按写权限检查
```

| 参数 | 取值 |
|---|---|
| `access` | `read`（默认）或 `write` |

用于观察当前页表状态下的地址翻译结果与权限检查行为。

---

## 12. `symbol-file` / `symbol` / `file`

```rvdb
symbol-file ./test_resources/bin/fib.elf
```

从指定 ELF 加载符号表，供 `break`、`ftrace`、`info symbols` 使用。

> 非 `native-cli` 构建（如 wasm）不支持从路径加载。

---

## 13. `info`

```rvdb
info breakpoints      # 别名：info b / bp / break
info symbols          # 别名：info sym / symbol
```

---

## 14. `quit` / `q` / `exit`

```rvdb
quit
```

退出 rvdb，同时结束模拟器进程。

快捷键：

| 按键 | 效果 |
|---|---|
| `Ctrl+D` | EOF，退出 |
| `Ctrl+C` | 中断，退出 |

---

## 15. 地址与寄存器写法

### 地址

| 写法 | 解释 |
|---|---|
| `1000` | 十进制 |
| `0x1000` | 十六进制（也支持大写 `0X`） |

> 注意：rvdb 的地址解析不接受下划线分隔（那是命令行 `--dtb-address` 的规则）。

### 物理 vs 虚拟

| 场景 | 默认 | 切换方式 |
|---|---|---|
| `p mem` | 物理地址 | `-v` |
| `break` | 物理地址 | `-v` |

### 寄存器名

- ABI 名：`zero`、`ra`、`sp`、`gp`、`tp`、`t0`-`t6`、`s0/fp`、`s1`-`s11`、`a0`-`a7`
- 数字名：`x0`-`x31`
- 浮点：`ft0`-`ft11`、`fs0`-`fs11`、`fa0`-`fa7`，或 `f0`-`f31`
- 向量：`v0`-`v31`

---

## 16. 输入技巧

### 重复上一条命令

输入空行即可重复上一条非空命令：

```rvdb
step
        ← 回车 = 再执行一次 step
        ← 回车 = 再执行一次 step
```

### 子命令帮助

```rvdb
print --help
print mem --help
break --help
ftrace --help
```

---

## 17. 典型工作流

### 17.1 观察程序启动

```rvdb
p pc
p regs
list
step
step
p reg sp
```

### 17.2 在函数入口停下

```rvdb
break main
continue
p pc
list
```

### 17.3 追踪递归调用

```rvdb
break main
continue
ftrace start
c 500
ftrace show 30
ftrace stat
```

### 17.4 观察异常与特权级

```rvdb
p priv
p csr mstatus
p csr mcause
p csr mepc
```

### 17.5 观察地址翻译

```rvdb
translate 0x1000
translate 0x1000 write
```

### 17.6 脚本化观察

`script.rvdb`：

```rvdb
break main
continue
p regs
ftrace start
c 1000
ftrace stat
history 20
quit
```

运行：

```bash
cargo run --profile fast-debug -- ./test_resources/bin/fib.elf -g -S ./script.rvdb
```

---

## 18. 注意事项

1. **rvdb 不改变寄存器初值**
   进入 rvdb 时程序尚未执行：PC 为 `0x80000000`（`DEFAULT_PC_VALUE`），通用寄存器为 0。

2. **每次运行都是全新机器**
   HERE 没有保存/恢复状态的功能，进程退出后 RAM、寄存器、CSR、设备状态全部丢弃。

3. **断点是 PC 比对，不是软件断点指令**
   不会写来宾内存，也不会影响程序行为。

4. **`p mem` 默认物理地址**
   这是最常见的误用来源。

5. **`history` 与 `ftrace` 的容量上限是 1024**
   超出后只保留最近的记录。

6. **非交互式环境必须使用 `-S`**
   否则 rvdb 会直接报错退出。

7. **退出模拟器**
   rvdb 内用 `quit`；非调试模式下用 `Ctrl+A` 松开再按 `x`。

---

## 19. 快速参考卡

```
┌─ 观察 ───────────────────────────────────────────────┐
│ p pc | p regs | p reg a0 | p mem ADDR -l N [-v]       │
│ p csr mstatus | p freg fa0 | p vreg v0 | p priv        │
│ d pc / d regs / undisplay pc                           │
│ list | history [N] | translate ADDR [write]            │
├─ 执行 ───────────────────────────────────────────────┤
│ step | continue | continue N                           │
├─ 断点 ───────────────────────────────────────────────┤
│ break SYM | break ADDR [-v] | break -d SYM             │
│ info breakpoints                                       │
├─ 追踪 ───────────────────────────────────────────────┤
│ ftrace start | stop | show N | stat                    │
├─ 其他 ───────────────────────────────────────────────┤
│ symbol-file ELF | info symbols | quit                  │
└──────────────────────────────────────────────────────┘
```
