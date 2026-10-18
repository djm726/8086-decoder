/*  8086 assembly decoder
 *  binary-> x86 asm
 */

#include "8086-decode.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <vector>

// check every literal field of an encoding against the bytes at code[i].
// literals all sit before any optional disp/data bytes, so stop there.
static bool matches(const Encoding& encoding, const std::vector<u8>& code,
                    size_t i)
{
    int pos = 0;
    for (int f = 0; f < 10 && encoding.fields[f].type != F_END; f++)
    {
        const BitField& field = encoding.fields[f];
        if (field.bit_cnt == 0)
            continue;
        if (field.type == F_DISP_LO || field.type == F_DISP_HI ||
            field.type == F_DATA_IF_W || field.type == F_DATA_IF_SW)
            break;

        size_t byte = i + pos / 8;
        if (byte >= code.size())
            return false;
        if (field.type == F_LITERAL)
        {
            u8 mask = (1 << field.bit_cnt) - 1;
            u8 value = (code[byte] >> (8 - pos % 8 - field.bit_cnt)) & mask;
            if (value != field.value)
                return false;
        }
        pos += field.bit_cnt;
    }
    return true;
}

bool decode(const std::vector<u8>& code, size_t& i, Encoding& curr)
{
    int bits = 0;
    const Encoding* match = nullptr;
    // run through all possible opcodes until match
    for (const auto& encoding : table)
    {
        if (matches(encoding, code, i))
        {
            match = &encoding;
            curr.op = encoding.op;
            curr.fields[0] = encoding.fields[0];
            bits += encoding.fields[0].bit_cnt;
            break;
        }
    }
    if (!match)
        return false;

    // read rest of encoding
    u8 mod = 0, rm = 0, w = 0, s = 0;
    int field_idx = 1;
    for (int f = 1; f < 10 && match->fields[f].type != F_END; f++)
    {
        const BitField& field = match->fields[f];

        // skip fields that aren't present in this instruction
        bool direct = (mod == 0b00 && rm == 0b110);
        if (field.type == F_DISP_LO && !(mod == 0b01 || mod == 0b10 || direct))
            continue;
        if (field.type == F_DISP_HI && !(mod == 0b10 || direct))
            continue;
        if (field.type == F_DATA_IF_W && !w)
            continue;
        if (field.type == F_DATA_IF_SW && !(w && !s))
            continue;

        // implicit fields consume no bits
        if (field.bit_cnt == 0)
        {
            curr.fields[field_idx++] = field;
            continue;
        }

        // finished the current byte, move to the next
        if (bits == 8)
        {
            i++;
            bits = 0;
        }

        // pull bit_cnt bits out of the current byte, msb first
        u8 mask = (1 << field.bit_cnt) - 1;
        u8 value = (code[i] >> (8 - bits - field.bit_cnt)) & mask;
        bits += field.bit_cnt;

        curr.fields[field_idx++] = {field.type, field.bit_cnt, value};

        switch (field.type)
        {
        case F_MOD: mod = value; break;
        case F_RM:  rm = value; break;
        case F_W:   w = value; break;
        case F_S:   s = value; break;
        default: break;
        }
    }
    // step past the last byte consumed
    if (bits > 0)
        i++;

    // terminate fields[]
    if (field_idx < 10)
        curr.fields[field_idx] = {F_END, 0, 0};

    decoded.push_back(curr);
    return true;
}

static Operand make_reg(u8 reg, bool wide)
{
    Operand o;
    o.type = OPND_REG;
    o.reg = reg;
    o.wide = wide;
    return o;
}

static Operand make_imm(i16 imm, bool wide)
{
    Operand o;
    o.type = OPND_IMM;
    o.imm = imm;
    o.wide = wide;
    return o;
}

Instruction to_instruction(const Encoding& enc)
{
    Instruction inst;
    inst.op = enc.op;

    // gather every field the encoding carries
    u8 d = 0, w = 0, s = 0, v = 0, mod = 0, reg = 0, rm = 0, sr = 0;
    u8 xxx = 0, yyy = 0;
    bool has_reg = false, has_rm = false, has_sr = false, has_v = false;
    bool has_dx = false, has_esc = false, has_addr = false;
    bool has_rel = false, has_far_addr = false;
    u16 disp = 0, data = 0, addr = 0, ip = 0, cs = 0;
    int disp_bytes = 0, data_bytes = 0;
    i16 rel = 0;

    for (int f = 0; f < 10 && enc.fields[f].type != F_END; f++)
    {
        const BitField& field = enc.fields[f];
        u8 value = field.value;
        switch (field.type)
        {
        case F_D:   d = value; break;
        case F_W:   w = value; if (w) inst.flags |= INST_WIDE; break;
        case F_S:   s = value; break;
        case F_V:   v = value; has_v = true; break;
        case F_Z:   if (value) inst.flags |= INST_Z; break;
        case F_MOD: mod = value; break;
        case F_REG: reg = value; has_reg = true; break;
        case F_RM:  rm = value; has_rm = true; break;
        case F_SR:  sr = value; has_sr = true; break;
        case F_DISP_LO: disp |= value; disp_bytes = 1; break;
        case F_DISP_HI: disp |= value << 8; disp_bytes = 2; break;
        case F_DATA:
        case F_DATA_LO: data |= value; data_bytes = 1; break;
        case F_DATA_IF_W:
        case F_DATA_IF_SW:
        case F_DATA_HI: data |= value << 8; data_bytes = 2; break;
        case F_ADDR_LO: addr |= value; has_addr = true; break;
        case F_ADDR_HI: addr |= value << 8; break;
        case F_IP_INC8: rel = (i8)value; has_rel = true; break;
        case F_IP_INC_LO: rel |= value; has_rel = true; break;
        case F_IP_INC_HI: rel |= value << 8; break;
        case F_IP_LO: ip |= value; has_far_addr = true; break;
        case F_IP_HI: ip |= value << 8; break;
        case F_CS_LO: cs |= value; break;
        case F_CS_HI: cs |= value << 8; break;
        case F_XXX: xxx = value; has_esc = true; break;
        case F_YYY: yyy = value; break;
        case F_DX:  has_dx = true; break;
        case F_FAR: if (value) inst.flags |= INST_FAR; break;
        default: break;
        }
    }

    // 8 bit displacement is sign extended
    i16 disp16 = disp_bytes == 1 ? (i8)disp : (i16)disp;
    // 8 bit data is sign extended to a word only when s = 1, w = 1
    i16 data16 = (data_bytes == 1 && s && w) ? (i8)data : (i16)data;
    Operand imm = make_imm(data16, w || data_bytes == 2);
    bool data_used = data_bytes == 0;

    // reg side: REG or SR field
    Operand reg_side;
    if (has_reg)
        reg_side = make_reg(reg, w);
    else if (has_sr)
    {
        reg_side.type = OPND_SEGREG;
        reg_side.reg = sr;
        reg_side.wide = true;
    }

    // other side: RM, direct address, DX, or a lone immediate/target
    Operand other;
    if (has_rm)
    {
        if (mod == 0b11)
            other = make_reg(rm, w);
        else
        {
            other.type = OPND_MEM;
            other.wide = w;
            other.rm = rm;
            other.direct = (mod == 0b00 && rm == 0b110);
            other.disp = disp16;
        }
    }
    else if (has_addr)
    {
        other.type = OPND_MEM;
        other.wide = w;
        other.direct = true;
        other.disp = (i16)addr;
    }
    else if (has_dx)
        other = make_reg((u8)reg16::dx, true);
    else if (has_rel)
    {
        other.type = OPND_REL;
        other.imm = rel;
    }
    else if (has_far_addr)
    {
        other.type = OPND_FAR_ADDR;
        other.imm = (i16)ip;
        other.cs = cs;
    }
    else if (!data_used)
    {
        other = imm;
        data_used = true;
    }

    // esc takes its opcode bits as an immediate before the r/m operand
    if (has_esc)
    {
        reg_side = make_imm((xxx << 3) | yyy, false);
        d = 1;
    }

    // d = 1: reg side first, d = 0: other side first
    Operand* dst = &inst.operands[0];
    Operand first = d ? reg_side : other;
    Operand second = d ? other : reg_side;
    if (first.type != OPND_NONE)
        *dst++ = first;
    if (second.type != OPND_NONE)
        *dst++ = second;

    // leftover immediate (e.g. mov r/m, imm) is the source
    if (!data_used && dst < inst.operands + 2)
        *dst++ = imm;

    // shift/rotate count: v = 1 -> cl, v = 0 -> 1
    if (has_v && dst < inst.operands + 2)
        *dst++ = v ? make_reg((u8)reg8::cl, false) : make_imm(1, false);

    return inst;
}

static const char* reg16_names[8] = {"ax", "cx", "dx", "bx",
                                     "sp", "bp", "si", "di"};
static const char* reg8_names[8] = {"al", "cl", "dl", "bl",
                                    "ah", "ch", "dh", "bh"};
static const char* sreg_names[4] = {"es", "cs", "ss", "ds"};
static const char* ea_base[8] = {"bx + si", "bx + di", "bp + si", "bp + di",
                                 "si",      "di",      "bp",      "bx"};

static std::string operand_str(const Operand& o, const char* prefix,
                               const std::string& seg, size_t size)
{
    switch (o.type)
    {
    case OPND_REG:
        return o.wide ? reg16_names[o.reg] : reg8_names[o.reg];
    case OPND_SEGREG:
        return sreg_names[o.reg & 3];
    case OPND_MEM:
    {
        std::string s = prefix + seg;
        if (o.direct)
            return s + "[" + std::to_string((u16)o.disp) + "]";
        s += std::string("[") + ea_base[o.rm];
        if (o.disp > 0)
            s += " + " + std::to_string(o.disp);
        if (o.disp < 0)
            s += " - " + std::to_string(-o.disp);
        return s + "]";
    }
    case OPND_IMM:
        return std::to_string(o.imm);
    case OPND_REL:
        // offset is from the end of the instruction, $ is its start
        return std::format("${:+}", (int)size + o.imm);
    case OPND_FAR_ADDR:
        return std::format("{}:{}", o.cs, (u16)o.imm);
    default:
        return "";
    }
}

// lock: "lock " to put in front of the line (or empty)
// seg: segment override like "cs:" to put in front of the memory operand
static void print_instruction(std::ofstream& out, const Instruction& inst,
                              size_t size, const std::string& lock,
                              const std::string& seg)
{
    std::string line = lock + ops[inst.op];
    switch (inst.op)
    {
    case OP_MOVS:
    case OP_CMPS:
    case OP_SCAS:
    case OP_LODS:
    case OP_STOS:
        line += (inst.flags & INST_WIDE) ? "w" : "b";
        break;
    case OP_RET:
        if (inst.flags & INST_FAR)
            line = "retf";
        break;
    case OP_REP:
        if (!(inst.flags & INST_Z))
            line = lock + "repne";
        break;
    case OP_INT:
        if (inst.operands[0].type == OPND_NONE)
            line = lock + "int3";
        break;
    default: break;
    }

    const Operand& a = inst.operands[0];
    const Operand& b = inst.operands[1];
    for (int n = 0; n < 2; n++)
    {
        const Operand& o = inst.operands[n];
        const Operand& other = n ? a : b;
        if (o.type == OPND_NONE)
            break;

        // memory needs an explicit size unless a register operand implies it
        // (a shift count in cl says nothing about the destination size)
        const char* prefix = "";
        bool is_shift = inst.op >= OP_SHL && inst.op <= OP_RCR;
        if (inst.flags & INST_FAR)
            prefix = "far ";
        else if (is_shift || (other.type != OPND_REG &&
                              other.type != OPND_SEGREG))
            prefix = o.wide ? "word " : "byte ";

        line += n ? ", " : " ";
        line += operand_str(o, prefix, seg, size);
    }
    std::println(out, "{}", line);
}

static std::ifstream open_input(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary);
    if (!file)
        std::println(stderr, "[8086-decode] Failed to open {}", filename);
    return file;
}

static std::ofstream open_output(const std::string& filename)
{
    std::ofstream file(filename);
    if (!file)
        std::println(stderr, "[8086-decode] Failed to create {}", filename);
    return file;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::println(stderr, "[8086-decode] Usage: $8086-decode <<bin name>>");
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream in = open_input(filename);
    if (!in)
        return 1;
    std::ofstream out = open_output(filename + "_new.asm");
    if (!out)
        return 1;

    // read input binary file into a vector
    std::vector<u8> code(std::filesystem::file_size(filename));
    in.read(reinterpret_cast<char*>(code.data()), code.size());

    // loop over binary instructions and write asm to output file
    // lock and segment overrides decode as their own instructions but print
    // as part of the instruction that follows them
    std::string lock, seg;
    std::size_t start = 0;
    std::size_t i = 0;
    while (i < code.size())
    {
        if (lock.empty() && seg.empty())
            start = i;
        Encoding curr = {};
        if (!decode(code, i, curr))
        {
            std::println(stderr, "[8086-decode] unknown op {:#04x}", code[i]);
            i++;
            continue;
        }
        Instruction inst = to_instruction(curr);
        if (inst.op == OP_LOCK)
        {
            lock = "lock ";
            continue;
        }
        if (inst.op == OP_SEGMENT)
        {
            seg = std::string(sreg_names[inst.operands[0].reg & 3]) + ":";
            continue;
        }
        print_instruction(out, inst, i - start, lock, seg);
        lock.clear();
        seg.clear();
    }

    return 0;
}
