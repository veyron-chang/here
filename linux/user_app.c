// user_app.c —— Chapter 6：运行在 HERE 模拟器内 Linux 用户态的程序
//
// 关键点：**不链接任何 libc**。
//
// 本机的 /opt/riscv 只有 newlib 的 `riscv64-unknown-elf` 交叉工具链（裸机语义），
// 用它 `-static` 编出来的程序在 Linux 下会因为 syscall 约定不对而崩。
// 所以这里直接按 RISC-V Linux syscall ABI 手写入口：
//
//   a7 = syscall number, a0..a5 = 参数, 返回值在 a0
//   write = 64, exit = 93
//
// glibc 的 crt0 起手式也是一模一样，只是它后面还要去调 __libc_start_main。
//
// 编译（见 build_user_app.sh）：
//   riscv64-unknown-elf-gcc -nostdlib -static -march=rv64gc -mabi=lp64d \
//       -mcmodel=medany -Wl,-Ttext=0x10000 -e _start user_app.c -o user_app

typedef unsigned long u64;
typedef long i64;

#define SYS_write 64
#define SYS_exit 93
#define SYS_getpid 172

/// 三个参数的 syscall 封装。返回值写回 a0。
static i64 sys_call3(i64 nr, i64 arg0, i64 arg1, i64 arg2) {
    register i64 a0 asm("a0") = arg0;
    register i64 a1 asm("a1") = arg1;
    register i64 a2 asm("a2") = arg2;
    register i64 a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static i64 sys_call0(i64 nr) {
    register i64 a0 asm("a0") = 0;
    register i64 a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static u64 str_len(const char *s) {
    u64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void put_str(const char *s) {
    sys_call3(SYS_write, 1 /* stdout */, (i64)s, (i64)str_len(s));
}

static void put_char(char c) {
    sys_call3(SYS_write, 1, (i64)&c, 1);
}

static void put_dec(u64 value) {
    char buf[24];
    int i = 0;
    if (value == 0) {
        put_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i-- > 0) {
        put_char(buf[i]);
    }
}

static void put_hex(u64 value, int width) {
    static const char digits[] = "0123456789abcdef";
    for (int shift = (width - 1) * 4; shift >= 0; shift -= 4) {
        put_char(digits[(value >> shift) & 0xF]);
    }
}

// 程序入口。Linux 在跳到这里时，栈顶布局是：
//   sp[0] = argc, sp[1..] = argv[], 之后是 envp[]
//
// 注意：不能在普通 C 函数体里读 sp —— GCC 会在序言里先 `addi sp, sp, -N`
// 建立栈帧，等函数体能执行时 sp 已经不是内核给的那个值了。
// 所以用 naked 函数在序言之前把原始 sp 抓进 a0，再 tail 到 C 函数。
void app_main(u64 *stack) __attribute__((used));

__attribute__((naked)) void _start(void) {
    __asm__ volatile("mv a0, sp");
    __asm__ volatile("tail app_main");
}

void app_main(u64 *stack) {
    u64 argc = stack[0];
    const char **argv = (const char **)&stack[1];

    put_str("=========================================\n");
    put_str(" Hello RISC-V Linux User Space!         \n");
    put_str(" Running inside RISC-V Emulator + Linux! \n");
    put_str("=========================================\n");

    put_str("[user_app] argc = ");
    put_dec(argc);
    put_str("\n");
    for (u64 i = 0; i < argc; i++) {
        put_str("[user_app] argv[");
        put_dec(i);
        put_str("] = ");
        put_str(argv[i]);
        put_str("\n");
    }

    put_str("[user_app] pid  = ");
    put_dec((u64)sys_call0(SYS_getpid));
    put_str("\n");

    // 地址能打出来，说明内核建好了 Sv39 用户地址空间。
    put_str("[user_app] _start 虚拟地址 = 0x");
    put_hex((u64)(void *)_start, 16);
    put_str("\n");

    put_str("[user_app] bye.\n");

    sys_call3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
