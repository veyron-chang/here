#include "io.h"
#include "log.h"
#include "power.h"
#include "custom_ops.h"
#include <stdarg.h>
#include <stdint.h>

// 定义 UART 内存映射地址 (QEMU virt 平台)
#define UART_ADDR 0x10000000
#define UART_LSR_ADDR (UART_ADDR + 0x05)
#define UART_LSR_THRE 0x20  // Bit 5: Transmitter Holding Register Empty

extern char starttext[], endtext[], startrodata[], endrodata[], startdata[], enddata[],
    startbss[], endbss[], stack_lower_bound[], stack_top[];

void display_section_info() {
    Log(INFO, ".text section: [%08x, %08x]", (uintptr_t)(starttext), (uintptr_t)endtext);
    Log(INFO, ".rodata section: [%08x, %08x]", (uintptr_t)startrodata,
        (uintptr_t)endrodata);
    Log(INFO, ".data section: [%08x, %08x]", (uintptr_t)startdata, (uintptr_t)enddata);
    Log(INFO, ".bas section: [%08x, %08x]", (uintptr_t)startbss, (uintptr_t)endbss);
    Log(INFO, ".stack section: [%08x, %08x]", (uintptr_t)stack_lower_bound,
        (uintptr_t)stack_top);
}

// =============================================================
// Chapter 3: 自定义指令的 Software Golden Model 与自动比对
// =============================================================

static uint32_t golden_padd8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t lane = ((a >> (8 * i)) & 0xFF) + ((b >> (8 * i)) & 0xFF);
        r |= (lane & 0xFF) << (8 * i);
    }
    return r;
}

static uint32_t golden_psub8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t lane = ((a >> (8 * i)) & 0xFF) - ((b >> (8 * i)) & 0xFF);
        r |= (lane & 0xFF) << (8 * i);
    }
    return r;
}

static uint32_t golden_padd8_sat(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t lane = ((a >> (8 * i)) & 0xFF) + ((b >> (8 * i)) & 0xFF);
        if (lane > 0xFF) {
            lane = 0xFF; // 饱和：钳位而不是回绕
        }
        r |= lane << (8 * i);
    }
    return r;
}

static uint32_t golden_pdot4(uint32_t a, uint32_t b) {
    uint32_t acc = 0;
    for (int i = 0; i < 4; i++) {
        acc += ((a >> (8 * i)) & 0xFF) * ((b >> (8 * i)) & 0xFF);
    }
    return acc;
}

typedef struct {
    uint32_t a;
    uint32_t b;
} CustomCase;

static const CustomCase CUSTOM_CASES[] = {
    {0x01020304, 0x05060708},
    {0xFF01FE02, 0x01FF02FE},
    {0x80808080, 0x80808080},
    {0x00000000, 0x00000000},
    {0xFFFFFFFF, 0xFFFFFFFF},
    {0x12345678, 0x87654321},
    {0x00FF00FF, 0x01000100},
    {0x7F7F7F7F, 0x01010101},
};

static int g_total = 0;
static int g_passed = 0;

static void check_one(const char* name, uint32_t a, uint32_t b, uint32_t hw, uint32_t sw) {
    g_total++;
    if (hw == sw) {
        g_passed++;
    } else {
        printf("[FAIL] %s a=%08x b=%08x hw=%08x sw=%08x\n", name, a, b, hw, sw);
    }
}

static void check_all_instructions(uint32_t a, uint32_t b) {
    check_one("padd8", a, b, padd8(a, b), golden_padd8(a, b));
    check_one("psub8", a, b, psub8(a, b), golden_psub8(a, b));
    check_one("padd8_sat", a, b, padd8_sat(a, b), golden_padd8_sat(a, b));
    check_one("pdot4", a, b, pdot4(a, b), golden_pdot4(a, b));
}

static int custom_verify_all(void) {
    int count = (int)(sizeof(CUSTOM_CASES) / sizeof(CUSTOM_CASES[0]));

    // 固定测试向量
    for (int i = 0; i < count; i++) {
        check_all_instructions(CUSTOM_CASES[i].a, CUSTOM_CASES[i].b);
    }

    // xorshift32 随机测试向量
    uint32_t state = 0x12345678u;
    for (int i = 0; i < 256; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        uint32_t a = state;

        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        uint32_t b = state;

        check_all_instructions(a, b);
    }

    printf("[custom] %d/%d checks passed\n", g_passed, g_total);
    return g_passed == g_total;
}

// 程序入口点 (由链接脚本指定)
int main() {
    TEST_START(__BASE_FILE__);
    display_section_info();
    printf("Hello Qemu.\nformat test: %8d %08x %4o %c %s\n", 1ll, 255ll, 15, 'c', "test");
    Log(ERROR, "error test.");
    Log(WARN, "warnning test.");
    Log(DEBUG, "debug test.");
    Log(INFO, "info test.");
    Log(TRACE, "trace test.");

    // Chapter 3：自定义指令后端执行单元 + Golden Model 比对
    int custom_ok = custom_verify_all();

    if (custom_ok) {
        pass();
    } else {
        fail();
    }

    return 0;
}