#pragma once

#include "8086-op.h"
#include <string>

// flags register bits, at their 8086 positions
enum Flag : u16 {
    FLAG_C = 1 << 0,  // carry
    FLAG_P = 1 << 2,  // parity
    FLAG_A = 1 << 4,  // auxiliary carry
    FLAG_Z = 1 << 6,  // zero
    FLAG_S = 1 << 7,  // sign
    FLAG_O = 1 << 11, // overflow
};

// General purpose registers, indexed by the 8086 reg encoding (see reg16).
// 8 bit registers alias the low/high bytes of ax-dx: reg & 3 picks the
// parent register and reg & 4 selects the high byte (see reg8).
struct Registers {
    u16 r[8];
    u16 sr[4]; // segment registers: es, cs, ss, ds
    u16 flags;

    u16 read(u8 reg, bool w) const
    {
        if (w)
            return r[reg];
        u16 v = r[reg & 3];
        return (reg & 4) ? v >> 8 : v & 0xFF;
    }

    void write(u8 reg, bool w, u16 val)
    {
        if (w)
        {
            r[reg] = val;
            return;
        }
        u16& v = r[reg & 3];
        if (reg & 4)
            v = (v & 0x00FF) | (val << 8);
        else
            v = (v & 0xFF00) | (val & 0xFF);
    }
};

// run one instruction against regs. returns false if it isn't supported yet
bool execute(Registers& regs, const Instruction& inst);

// execute inst, printing the destination register before and after
bool verify(Registers& regs, const Instruction& inst, const std::string& text);
