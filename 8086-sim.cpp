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

// offset part of a memory operand's effective address, with its default
// segment: ss for bp based addressing, ds otherwise
static u16 effective_address(const Registers& regs, const Operand& o, u8& seg)
{
    using enum reg16;
    auto r = [&](reg16 reg) { return regs.r[(u8)reg]; };
    seg = SR_DS;
    if (o.direct)
        return (u16)o.disp;
    u16 base = 0;
    switch (o.rm)
    {
    case 0: base = r(bx) + r(si); break;
    case 1: base = r(bx) + r(di); break;
    case 2: base = r(bp) + r(si); seg = SR_SS; break;
    case 3: base = r(bp) + r(di); seg = SR_SS; break;
    case 4: base = r(si); break;
    case 5: base = r(di); break;
    case 6: base = r(bp); seg = SR_SS; break;
    case 7: base = r(bx); break;
    }
    return base + o.disp;
}

// segment and offset a memory operand addresses, honoring an override
static void mem_address(const Registers& regs, const Operand& o, u8 override,
                        u16& seg, u16& off)
{
    u8 sr;
    off = effective_address(regs, o, sr);
    seg = regs.sr[override == SR_NONE ? sr : override];
}

// value of a source operand
static bool read_operand(const Registers& regs, const Memory& mem,
                         const Operand& o, u8 override, u16& val)
{
    switch (o.type)
    {
    case OPND_REG:
        val = regs.read(o.reg, o.wide);
        return true;
    case OPND_SEGREG:
        val = regs.sr[o.reg & 3];
        return true;
    case OPND_MEM: {
        u16 seg, off;
        mem_address(regs, o, override, seg, off);
        val = mem.read(seg, off, o.wide);
        return true;
    }
    case OPND_IMM:
        val = (u16)o.imm;
        return true;
    default:
        return false;
    }
}

static bool write_operand(Registers& regs, Memory& mem, const Operand& o,
                          u8 override, u16 val)
{
    switch (o.type)
    {
    case OPND_REG:
        regs.write(o.reg, o.wide, val);
        return true;
    case OPND_SEGREG:
        regs.sr[o.reg & 3] = val;
        return true;
    case OPND_MEM: {
        u16 seg, off;
        mem_address(regs, o, override, seg, off);
        mem.write(seg, off, o.wide, val);
        return true;
    }
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

// whether a conditional jump or loop is taken. loops decrement cx first
static bool branch_taken(Registers& regs, OpType op)
{
    u16 f = regs.flags;
    bool c = f & FLAG_C, p = f & FLAG_P, z = f & FLAG_Z;
    bool s = f & FLAG_S, o = f & FLAG_O;
    u16& cx = regs.r[(u8)reg16::cx];
    switch (op)
    {
    case OP_JE:     return z;
    case OP_JL:     return s != o;
    case OP_JLE:    return z || s != o;
    case OP_JB:     return c;
    case OP_JBE:    return c || z;
    case OP_JP:     return p;
    case OP_JO:     return o;
    case OP_JS:     return s;
    case OP_JNZ:    return !z;
    case OP_JNL:    return s == o;
    case OP_JNLE:   return !z && s == o;
    case OP_JNB:    return !c;
    case OP_JNBE:   return !c && !z;
    case OP_JNP:    return !p;
    case OP_JNO:    return !o;
    case OP_JNS:    return !s;
    case OP_LOOP:   return --cx != 0;
    case OP_LOOPZ:  return --cx != 0 && z;
    case OP_LOOPNZ: return --cx != 0 && !z;
    case OP_JCXZ:   return cx == 0;
    default:        return false;
    }
}

static bool is_branch(OpType op)
{
    return op >= OP_JE && op <= OP_JCXZ;
}

static bool is_loop(OpType op)
{
    return op == OP_LOOP || op == OP_LOOPZ || op == OP_LOOPNZ;
}

bool execute(Registers& regs, Memory& mem, const Instruction& inst, u8 seg)
{
    const Operand& dst = inst.operands[0];
    const Operand& src = inst.operands[1];
    switch (inst.op)
    {
    case OP_MOV: {
        u16 val;
        return read_operand(regs, mem, src, seg, val) &&
               write_operand(regs, mem, dst, seg, val);
    }
    case OP_ADD:
    case OP_SUB:
    case OP_CMP: {
        u16 a, b;
        if (!read_operand(regs, mem, dst, seg, a) ||
            !read_operand(regs, mem, src, seg, b))
            return false;
        u16 res = arith(regs, inst.op == OP_ADD ? OP_ADD : OP_SUB, a, b,
                        dst.wide);
        return inst.op == OP_CMP || write_operand(regs, mem, dst, seg, res);
    }
    default:
        if (!is_branch(inst.op) || dst.type != OPND_REL)
            return false;
        if (branch_taken(regs, inst.op))
            regs.ip += dst.imm;
        return true;
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

// clocks to compute a memory operand's effective address
static int ea_clocks(const Operand& o, u8 seg)
{
    // indexed by rm. bx+si and bp+di take 7, bx+di and bp+si take 8
    static const int base_index[8] = {7, 8, 8, 7, 5, 5, 5, 5};
    int clocks = o.direct ? 6 : base_index[o.rm] + (o.disp ? 4 : 0);
    return clocks + (seg == SR_NONE ? 0 : 2);
}

static bool is_acc(const Operand& o)
{
    return o.type == OPND_REG && o.reg == 0;
}

// clocks for inst from the 8086 manual, excluding ea and transfer penalties.
// taken: whether a jump/loop branched. has_ea: whether the memory operand
// (if any) adds ea clocks
static int base_clocks(const Instruction& inst, bool taken, bool& has_ea)
{
    const Operand& dst = inst.operands[0];
    const Operand& src = inst.operands[1];
    bool mem_dst = dst.type == OPND_MEM, mem_src = src.type == OPND_MEM;
    has_ea = mem_dst || mem_src;
    switch (inst.op)
    {
    case OP_MOV:
        // mov al/ax to/from a direct address has its own short encoding
        if ((mem_dst && src.type == OPND_REG) ||
            (mem_src && dst.type == OPND_REG))
        {
            const Operand& reg = mem_dst ? src : dst;
            const Operand& m = mem_dst ? dst : src;
            if (is_acc(reg) && m.direct)
            {
                has_ea = false;
                return 10;
            }
        }
        if (mem_dst)
            return src.type == OPND_IMM ? 10 : 9;
        if (mem_src)
            return 8;
        return src.type == OPND_IMM ? 4 : 2;
    case OP_ADD:
    case OP_SUB:
        if (mem_dst)
            return src.type == OPND_IMM ? 17 : 16;
        if (mem_src)
            return 9;
        return src.type == OPND_IMM ? 4 : 3;
    case OP_CMP:
        if (mem_dst)
            return src.type == OPND_IMM ? 10 : 9;
        if (mem_src)
            return 9;
        return src.type == OPND_IMM ? 4 : 3;
    case OP_LOOP:   return taken ? 17 : 5;
    case OP_LOOPZ:  return taken ? 18 : 6;
    case OP_LOOPNZ: return taken ? 19 : 5;
    case OP_JCXZ:   return taken ? 18 : 6;
    default:        return is_branch(inst.op) ? (taken ? 16 : 4) : 0;
    }
}

// clocks lost to word transfers. the 8086 splits a word at an odd address
// into two bus cycles, the 8088 always does. off: the memory operand's offset
static int transfer_penalty(const Instruction& inst, u16 off, bool i8088)
{
    const Operand& dst = inst.operands[0];
    const Operand& m = dst.type == OPND_MEM ? dst : inst.operands[1];
    if (m.type != OPND_MEM || !m.wide || (!i8088 && !(off & 1)))
        return 0;
    // read-modify-write destinations transfer twice
    bool rmw = dst.type == OPND_MEM && (inst.op == OP_ADD || inst.op == OP_SUB);
    return rmw ? 8 : 4;
}

static std::string clocks_str(Timing& timing, const Instruction& inst, u8 seg,
                              u16 off, bool taken)
{
    const Operand& dst = inst.operands[0];
    const Operand& m = dst.type == OPND_MEM ? dst : inst.operands[1];
    bool has_ea;
    int base = base_clocks(inst, taken, has_ea);
    int ea = has_ea ? ea_clocks(m, seg) : 0;
    int penalty = transfer_penalty(inst, off, timing.i8088);
    int clocks = base + ea + penalty;
    timing.total += clocks;

    std::string s = std::format(" Clocks: +{} = {}", clocks, timing.total);
    if (timing.explain && (ea || penalty))
    {
        s += std::format(" ({}", base);
        if (ea)
            s += std::format(" + {}ea", ea);
        if (penalty)
            s += std::format(" + {}p", penalty);
        s += ")";
    }
    return s + " |";
}

bool verify(Registers& regs, Memory& mem, Timing& timing,
            const Instruction& inst, u8 seg, std::size_t size,
            const std::string& text)
{
    const char* name = nullptr;
    // cmp only sets flags, so don't report its destination register.
    // loops implicitly count down cx
    u16* dst = nullptr;
    if (is_loop(inst.op))
    {
        name = reg16_names[(u8)reg16::cx];
        dst = &regs.r[(u8)reg16::cx];
    } else if (inst.op != OP_CMP)
        dst = dest_register(regs, inst.operands[0], name);
    u16 before = dst ? *dst : 0;
    u16 flags_before = regs.flags;
    u16 ip_before = regs.ip;
    regs.ip += size;

    // the memory operand's offset, before execution can change its registers
    u16 off = 0;
    for (const Operand& o : inst.operands)
    {
        u16 seg_val;
        if (o.type == OPND_MEM)
            mem_address(regs, o, seg, seg_val, off);
    }

    if (!execute(regs, mem, inst, seg))
    {
        std::println("{} ; unsupported", text);
        return false;
    }

    std::string trace;
    if (timing.show)
        trace += clocks_str(timing, inst, seg, off,
                            regs.ip != (u16)(ip_before + size));
    if (dst && *dst != before)
        trace += std::format(" {}:{:#x}->{:#x}", name, before, *dst);
    trace += std::format(" ip:{:#x}->{:#x}", ip_before, regs.ip);
    if (regs.flags != flags_before)
        trace += std::format(" flags:{}->{}", flags_str(flags_before),
                             flags_str(regs.flags));
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
    std::println("      ip: {:#06x} ({})", regs.ip, regs.ip);
    if (regs.flags)
        std::println("   flags: {}", flags_str(regs.flags));
}

int main(int argc, char** argv)
{
    // flags come before the binary
    Timing timing;
    bool dump = false;
    std::string filename;
    for (int a = 1; a < argc; a++)
    {
        std::string arg = argv[a];
        if (arg == "-showclocks")
            timing.show = true;
        else if (arg == "-explainclocks")
            timing.show = timing.explain = true;
        else if (arg == "-8088")
            timing.i8088 = true;
        else if (arg == "-dump")
            dump = true;
        else if (filename.empty() && !arg.starts_with("-"))
            filename = arg;
        else
            filename.clear(), a = argc;
    }
    if (filename.empty())
    {
        std::println(stderr, "[8086-sim] Usage: $8086-sim [-showclocks] "
                             "[-explainclocks] [-8088] [-dump] <<bin name>>");
        return 1;
    }

    std::ifstream in(filename, std::ios::binary);
    if (!in)
    {
        std::println(stderr, "[8086-sim] Failed to open {}", filename);
        return 1;
    }

    // load the program into memory at cs:0
    Registers regs = {};
    Memory mem;
    std::size_t code_size = std::filesystem::file_size(filename);
    if (code_size > mem.bytes.size())
    {
        std::println(stderr, "[8086-sim] {} doesn't fit in memory", filename);
        return 1;
    }
    in.read(reinterpret_cast<char*>(mem.bytes.data()), code_size);

    // decode and execute the instruction at cs:ip until it runs off the end.
    // lock and segment overrides decode as their own instructions but apply
    // to (and print with) the instruction that follows them
    std::string lock;
    u8 seg = SR_NONE;
    u16 start = 0;
    while (regs.ip < code_size)
    {
        if (lock.empty() && seg == SR_NONE)
            start = regs.ip;
        std::size_t i = Memory::physical(regs.sr[SR_CS], regs.ip);
        std::size_t begin = i;
        Encoding curr = {};
        if (!decode(mem.bytes, i, curr))
        {
            std::println(stderr, "[8086-sim] unknown op {:#04x}",
                         mem.bytes[begin]);
            regs.ip++;
            continue;
        }
        regs.ip += i - begin;
        Instruction inst = to_instruction(curr);
        if (inst.op == OP_LOCK)
        {
            lock = "lock ";
            continue;
        }
        if (inst.op == OP_SEGMENT)
        {
            seg = inst.operands[0].reg & 3;
            continue;
        }

        // verify advances ip itself, so rewind to the first prefix
        std::size_t size = regs.ip - start;
        regs.ip = start;
        std::string seg_str =
            seg == SR_NONE ? "" : std::string(sreg_names[seg]) + ":";
        verify(regs, mem, timing, inst, seg, size,
               instruction_str(inst, size, lock, seg_str));
        lock.clear();
        seg = SR_NONE;
    }

    print_registers(regs);

    // raw 1MB memory image, e.g. to view what a program drew
    if (dump)
    {
        std::string dump_name = filename + "_memory.data";
        std::ofstream out(dump_name, std::ios::binary);
        if (!out)
        {
            std::println(stderr, "[8086-sim] Failed to create {}", dump_name);
            return 1;
        }
        out.write(reinterpret_cast<const char*>(mem.bytes.data()),
                  mem.bytes.size());
    }
    return 0;
}
