/*  8086 simulator
 *  executes decoded 8086 instructions
 */

#include "8086-sim.h"
#include "8086-decode.h"
#include <bit>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <utility>
#include <vector>

static const char* reg16_names[8] = {"ax", "cx", "dx", "bx",
                                     "sp", "bp", "si", "di"};
static const char* sreg_names[4] = {"es", "cs", "ss", "ds"};

// value of a source operand. only registers and immediates for now
static bool read_operand(const Registers& regs, const Operand& o, u16& val)
{
    switch (o.type)
    {
    case OPND_REG:
        val = regs.read(o.reg, o.wide);
        return true;
    case OPND_SEGREG:
        val = regs.sr[o.reg & 3];
        return true;
    case OPND_IMM:
        val = (u16)o.imm;
        return true;
    default:
        return false;
    }
}

static bool write_operand(Registers& regs, const Operand& o, u16 val)
{
    switch (o.type)
    {
    case OPND_REG:
        regs.write(o.reg, o.wide, val);
        return true;
    case OPND_SEGREG:
        regs.sr[o.reg & 3] = val;
        return true;
    default:
        return false;
    }
}

// add/sub dst and src at the operand width, setting the arithmetic flags
static u16 arith(Registers& regs, OpType op, u16 dst, u16 src, bool w)
{
    u32 mask = w ? 0xFFFF : 0xFF;
    u16 sign = w ? 0x8000 : 0x80;
    dst &= mask;
    src &= mask;

    bool add = op == OP_ADD;
    u32 full = add ? (u32)dst + src : (u32)dst - src;
    u16 res = full & mask;

    u16 f = 0;
    if (full & (mask + 1))
        f |= FLAG_C;
    if (std::popcount((u8)res) % 2 == 0)
        f |= FLAG_P;
    if ((dst ^ src ^ res) & 0x10)
        f |= FLAG_A;
    if (res == 0)
        f |= FLAG_Z;
    if (res & sign)
        f |= FLAG_S;
    // add: operands share a sign the result lacks
    // sub: operands differ in sign and the result's sign differs from dst
    u16 ovf = add ? ~(dst ^ src) & (dst ^ res) : (dst ^ src) & (dst ^ res);
    if (ovf & sign)
        f |= FLAG_O;

    const u16 arith_flags = FLAG_C | FLAG_P | FLAG_A | FLAG_Z | FLAG_S | FLAG_O;
    regs.flags = (regs.flags & ~arith_flags) | f;
    return res;
}

bool execute(Registers& regs, const Instruction& inst)
{
    const Operand& dst = inst.operands[0];
    const Operand& src = inst.operands[1];
    switch (inst.op)
    {
    case OP_MOV: {
        u16 val;
        return read_operand(regs, src, val) && write_operand(regs, dst, val);
    }
    case OP_ADD:
    case OP_SUB:
    case OP_CMP: {
        u16 a, b;
        if (!read_operand(regs, dst, a) || !read_operand(regs, src, b))
            return false;
        u16 res = arith(regs, inst.op == OP_ADD ? OP_ADD : OP_SUB, a, b,
                        dst.wide);
        return inst.op == OP_CMP || write_operand(regs, dst, res);
    }
    default:
        return false;
    }
}

// full 16 bit register a destination operand lives in (al -> ax)
static u16* dest_register(Registers& regs, const Operand& o, const char*& name)
{
    switch (o.type)
    {
    case OPND_REG: {
        u8 reg = o.wide ? o.reg : o.reg & 3;
        name = reg16_names[reg];
        return &regs.r[reg];
    }
    case OPND_SEGREG:
        name = sreg_names[o.reg & 3];
        return &regs.sr[o.reg & 3];
    default:
        return nullptr;
    }
}

// set flags as letters, in reference output order
static std::string flags_str(u16 flags)
{
    static const std::pair<Flag, char> names[] = {
        {FLAG_C, 'C'}, {FLAG_P, 'P'}, {FLAG_A, 'A'},
        {FLAG_Z, 'Z'}, {FLAG_S, 'S'}, {FLAG_O, 'O'}};
    std::string s;
    for (auto [flag, c] : names)
        if (flags & flag)
            s += c;
    return s;
}

bool verify(Registers& regs, const Instruction& inst, const std::string& text)
{
    const char* name = nullptr;
    // cmp only sets flags, so don't report its destination register
    u16* dst = inst.op == OP_CMP
                   ? nullptr
                   : dest_register(regs, inst.operands[0], name);
    u16 before = dst ? *dst : 0;
    u16 flags_before = regs.flags;

    if (!execute(regs, inst))
    {
        std::println("{} ; unsupported", text);
        return false;
    }

    std::string trace;
    if (dst)
        trace += std::format(" {}:{:#x}->{:#x}", name, before, *dst);
    if (regs.flags != flags_before)
        trace += std::format(" flags:{}->{}", flags_str(flags_before),
                             flags_str(regs.flags));
    if (trace.empty())
        std::println("{}", text);
    else
        std::println("{} ;{}", text, trace);
    return true;
}

static void print_registers(const Registers& regs)
{
    // a-d then pointer/index, not encoding order (matches reference output)
    static const reg16 order[8] = {reg16::ax, reg16::bx, reg16::cx, reg16::dx,
                                   reg16::sp, reg16::bp, reg16::si, reg16::di};
    std::println("\nFinal registers:");
    for (reg16 reg : order)
    {
        u8 i = (u8)reg;
        if (regs.r[i])
            std::println("      {}: {:#06x} ({})", reg16_names[i], regs.r[i],
                         regs.r[i]);
    }
    for (int i = 0; i < 4; i++)
        if (regs.sr[i])
            std::println("      {}: {:#06x} ({})", sreg_names[i], regs.sr[i],
                         regs.sr[i]);
    if (regs.flags)
        std::println("   flags: {}", flags_str(regs.flags));
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::println(stderr, "[8086-sim] Usage: $8086-sim <<bin name>>");
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream in(filename, std::ios::binary);
    if (!in)
    {
        std::println(stderr, "[8086-sim] Failed to open {}", filename);
        return 1;
    }

    // read input binary file into a vector
    std::vector<u8> code(std::filesystem::file_size(filename));
    in.read(reinterpret_cast<char*>(code.data()), code.size());

    // decode and execute each instruction in order
    Registers regs = {};
    std::size_t i = 0;
    while (i < code.size())
    {
        std::size_t start = i;
        Encoding curr = {};
        if (!decode(code, i, curr))
        {
            std::println(stderr, "[8086-sim] unknown op {:#04x}", code[i]);
            i++;
            continue;
        }
        Instruction inst = to_instruction(curr);
        verify(regs, inst, instruction_str(inst, i - start, "", ""));
    }

    print_registers(regs);
    return 0;
}
