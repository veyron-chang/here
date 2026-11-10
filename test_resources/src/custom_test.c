// custom_test.c —— 自定义指令 padd8 的裸机端到端测试
//
// 运行前必须用带 custom-instr feature 的模拟器：
//   cargo run --features custom-instr -- ./test_resources/bin/custom_test.elf
//
// 预期输出：
//   padd8(01020304, 05060708) = 06080a0c
//   padd8 PASS

#include "custom_ops.h"
#include "io.h"
#include "power.h"
#include <stdint.h>

int main(void) {
    TEST_START(__BASE_FILE__);

    uint32_t a = 0x01020304u;
    uint32_t b = 0x05060708u;
    uint32_t expected = 0x06080A0Cu;

    uint32_t result = padd8(a, b);

    printf("padd8(%08x, %08x) = %08x\n", a, b, result);

    if (result == expected) {
        printf("padd8 PASS\n");
        pass();
    } else {
        printf("padd8 FAIL: expected %08x\n", expected);
        fail();
    }

    PowerOff();
    return 0;
}
