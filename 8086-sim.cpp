/*  8086 simulator
 *  executes decoded 8086 instructions
 */

#include "8086-sim.h"
#include "8086-decode.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
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

bool verify(Registers& regs, const Instruction& inst, const std::string& text)
{
    const char* name = nullptr;
    u16* dst = dest_register(regs, inst.operands[0], name);
    u16 before = dst ? *dst : 0;

    if (!execute(regs, inst))
    {
        std::println("{} ; unsupported", text);
        return false;
    }

    if (dst)
        std::println("{} ; {}:{:#x}->{:#x}", text, name, before, *dst);
    else
        std::println("{}", text);
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
