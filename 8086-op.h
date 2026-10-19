#pragma once

#include <cstdint>

using u8 = uint8_t;
using i8 = int8_t;
using i16 = int16_t;
using u16 = uint16_t;
using u32 = uint32_t;

enum OpType {
    OP_MOV,
    OP_PUSH,
    OP_POP,
    OP_XCHG,
    OP_IN,
    OP_OUT,
    OP_XLAT,
    OP_LEA,
    OP_LDS,
    OP_LES,
    OP_LAHF,
    OP_SAHF,
    OP_PUSHF,
    OP_POPF,
    OP_ADD,
    OP_ADC,
    OP_INC,
    OP_AAA,
    OP_DAA,
    OP_SUB,
    OP_SBB,
    OP_DEC,
    OP_NEG,
    OP_CMP,
    OP_AAS,
    OP_DAS,
    OP_MUL,
    OP_IMUL,
    OP_AAM,
    OP_DIV,
    OP_IDIV,
    OP_AAD,
    OP_CBW,
    OP_CWD,
    OP_NOT,
    OP_SHL,
    OP_SHR,
    OP_SAR,
    OP_ROL,
    OP_ROR,
    OP_RCL,
    OP_RCR,
    OP_AND,
    OP_TEST,
    OP_OR,
    OP_XOR,
    OP_REP,
    OP_MOVS,
    OP_CMPS,
    OP_SCAS,
    OP_LODS,
    OP_STOS,
    OP_CALL,
    OP_JMP,
    OP_RET,
    OP_JE,
    OP_JL,
    OP_JLE,
    OP_JB,
    OP_JBE,
    OP_JP,
    OP_JO,
    OP_JS,
    OP_JNZ,
    OP_JNL,
    OP_JNLE,
    OP_JNB,
    OP_JNBE,
    OP_JNP,
    OP_JNO,
    OP_JNS,
    OP_LOOP,
    OP_LOOPZ,
    OP_LOOPNZ,
    OP_JCXZ,
    OP_INT,
    OP_INTO,
    OP_IRET,
    OP_CLC,
    OP_CMC,
    OP_STC,
    OP_CLD,
    OP_STD,
    OP_CLI,
    OP_STI,
    OP_HLT,
    OP_WAIT,
    OP_ESC,
    OP_LOCK,
    OP_SEGMENT,
    OP_COUNT
};

static const char *ops[OP_COUNT] = {
    "mov",  "push", "pop",  "xchg", "in",    "out",  "xlat",  "lea",
    "lds",  "les",  "lahf", "sahf", "pushf", "popf", "add",   "adc",
    "inc",  "aaa",  "daa",  "sub",  "sbb",  "dec",  "neg",   "cmp",  "aas",   "das",
    "mul",  "imul", "aam",  "div",  "idiv",  "aad",  "cbw",   "cwd",
    "not",  "shl",  "shr",  "sar",  "rol",   "ror",  "rcl",   "rcr",
    "and",  "test", "or",   "xor",  "rep",   "movs", "cmps",  "scas",
    "lods", "stos", "call", "jmp",  "ret",   "je",   "jl",    "jle",
    "jb",   "jbe",  "jp",   "jo",   "js",    "jnz",  "jnl",   "jnle",
    "jnb",  "jnbe", "jnp",  "jno",  "jns",   "loop", "loopz", "loopnz",
    "jcxz", "int",  "into", "iret", "clc",   "cmc",  "stc",   "cld",
    "std",  "cli",  "sti",  "hlt",  "wait",  "esc",  "lock",  "segment"};

enum class reg16 : u8 {
    ax,
    cx,
    dx,
    bx,
    sp,
    bp,
    si,
    di,
};

enum class reg8 : u8 {
    al,
    cl,
    dl,
    bl,
    ah,
    ch,
    dh,
    bh,
};

enum OperandType : u8 {
    OPND_NONE,
    OPND_REG,      // general register: reg + wide pick reg8/reg16
    OPND_SEGREG,   // segment register: es, cs, ss, ds
    OPND_MEM,      // effective address: rm + disp, or direct address
    OPND_IMM,      // immediate data
    OPND_REL,      // ip-relative jump/call offset
    OPND_FAR_ADDR, // direct intersegment cs:ip
};

struct Operand {
    OperandType type = OPND_NONE;
    bool wide = false; // word (1) or byte (0) sized operand
    u8 reg = 0;        // OPND_REG / OPND_SEGREG index
    u8 rm = 0;         // OPND_MEM: base/index combination
    bool direct = false; // OPND_MEM: disp is a direct address, rm unused
    i16 disp = 0;      // OPND_MEM displacement / direct address
    i16 imm = 0;       // OPND_IMM value, OPND_REL offset, OPND_FAR_ADDR ip
    bool sign_ext = false; // OPND_IMM: byte sign extended to a word (s = 1)
    u16 cs = 0;        // OPND_FAR_ADDR segment
};

enum InstFlags : u8 {
    INST_FAR = 1 << 0, // intersegment call/jmp/ret
    INST_Z = 1 << 1,   // rep z bit: rep/repe (1) vs repne (0)
    INST_WIDE = 1 << 2, // w bit set, for operand-less movsw/stosw/...
};

struct Instruction {
    OpType op = OP_COUNT;
    Operand operands[2];
    u8 flags = 0;
};
