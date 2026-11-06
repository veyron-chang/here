#pragma once

#include <stdint.h>

/*
 * 自定义指令 padd8 的 C 语言封装。
 *
 * 编码（R 型，custom-0）：
 *   opcode = 0x0B
 *   funct3 = 0
 *   funct7 = 0
 *   mask   = 0xFE00707F
 *   match  = 0x0B
 *
 * 汇编器不认识 padd8 这个名字，因此用 .insn 直接拼机器码：
 *   .insn r opcode, funct3, funct7, rd, rs1, rs2
 *
 * 语义：4 路 8-bit 打包加法，每个 lane 独立回绕。
 */
static inline uint32_t padd8(uint32_t a, uint32_t b) {
    uint32_t r;
    asm volatile(".insn r 0x0b, 0, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
