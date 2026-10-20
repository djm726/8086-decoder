#pragma once

#include "8086-op.h"
#include <string>
#include <vector>

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
    u16 ip;

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

// segment register indices into Registers::sr
enum Sreg : u8 { SR_ES, SR_CS, SR_SS, SR_DS, SR_NONE = 0xFF };

// the full 1MB 20 bit physical address space
struct Memory {
    std::vector<u8> bytes = std::vector<u8>(1 << 20);

    // seg:off to a physical address, wrapping at 1MB like the 8086
    static u32 physical(u16 seg, u16 off)
    {
        return (((u32)seg << 4) + off) & 0xFFFFF;
    }

    // words are little endian. the high byte's offset wraps within the
    // segment
    u16 read(u16 seg, u16 off, bool w) const
    {
        u16 lo = bytes[physical(seg, off)];
        return w ? lo | (bytes[physical(seg, off + 1)] << 8) : lo;
    }

    void write(u16 seg, u16 off, bool w, u16 val)
    {
        bytes[physical(seg, off)] = val & 0xFF;
        if (w)
            bytes[physical(seg, off + 1)] = val >> 8;
    }
};

// run one instruction against regs and mem. seg is a segment override
// prefix's register, or SR_NONE. returns false if it isn't supported yet.
// regs.ip must already point past inst, as jumps are relative to that
bool execute(Registers& regs, Memory& mem, const Instruction& inst, u8 seg);

// clock estimation settings and the running total
struct Timing {
    bool show = false;    // print each instruction's clocks
    bool explain = false; // break clocks into base + ea + penalties
    bool i8088 = false;   // 8 bit bus: every word transfer pays a penalty
    u32 total = 0;
};

// advance ip past inst (size bytes, including any prefixes) and execute it,
// printing its clocks and the destination register, ip and flags before and
// after
bool verify(Registers& regs, Memory& mem, Timing& timing,
            const Instruction& inst, u8 seg, std::size_t size,
            const std::string& text);
