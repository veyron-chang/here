#pragma once

#include <stdint.h>

/*
 * Chapter 3 自定义 Packed-SIMD 指令的 C 封装。
 *
 * 全部为 R 型、custom-0 (opcode = 0x0B)、funct7 = 0，仅 funct3 不同：
 *
 *   指令         funct3   match
 *   padd8          0      0x0B
 *   psub8          1      0x100B
 *   padd8_sat      2      0x200B
 *   pdot4          3      0x300B
 *
 * 汇编器不认识这些名字，因此用 .insn 直接拼机器码：
 *   .insn r opcode, funct3, funct7, rd, rs1, rs2
 */

/// 逐 lane 8-bit 回绕加法（进位隔离）。
static inline uint32_t padd8(uint32_t a, uint32_t b) {
    uint32_t r;
    asm volatile(".insn r 0x0b, 0, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/// 逐 lane 8-bit 回绕减法（借位隔离）。
static inline uint32_t psub8(uint32_t a, uint32_t b) {
    uint32_t r;
    asm volatile(".insn r 0x0b, 1, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/// 逐 lane 8-bit 无符号饱和加法（溢出钳位到 255）。
static inline uint32_t padd8_sat(uint32_t a, uint32_t b) {
    uint32_t r;
    asm volatile(".insn r 0x0b, 2, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/// 低 4 个字节的 8-bit 点积，结果零扩展到 XLEN。
static inline uint32_t pdot4(uint32_t a, uint32_t b) {
    uint32_t r;
    asm volatile(".insn r 0x0b, 3, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
