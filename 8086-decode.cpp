/*  8086 assembly decoder
 *  binary-> x86 asm
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <vector>

static const char *reg16[8] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
static const char *reg8[8] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
static const char *eaBase[8] = {"bx + si", "bx + di", "bp + si", "bp + di",
                                "si",      "di",      "bp",      "bx"};
static const char *arithCode[8] = {"add", "or",  "adc", "sbb",
                                   "and", "sub", "xor", "cmp"};

static int read16(const std::vector<uint8_t> &code, size_t &i) {
    int v = code[i] | (code[i + 1] << 8);
    i += 2;
    return v;
}

static const char *regName(int r, bool w) { return w ? reg16[r] : reg8[r]; }

using DecodeFn = void (*)(const char *mnemonic, uint8_t op,
                          const std::vector<uint8_t> &code, size_t &i,
                          std::ofstream &out);

struct OpInfo {
    const char *mnemonic = nullptr;
    DecodeFn decode = nullptr;
};

// Immediate to register
static void decodeRegImm(const char *m, uint8_t op,
                         const std::vector<uint8_t> &code, size_t &i,
                         std::ofstream &out) {
    bool w = (op >> 3) & 1;
    int reg = op & 7;
    int imm = w ? (int16_t)read16(code, i) : (int8_t)code[i++];
    std::println(out, "{} {}, {}", m, regName(reg, w), imm);
}

static void decodeModRM(const char *m, uint8_t op,
                        const std::vector<uint8_t> &code, size_t &i,
                        std::ofstream &out) {
    bool w = op & 1;
    bool d = (op >> 1) & 1;
    uint8_t modrm = code[i++];
    int mod = modrm >> 6;
    int reg = (modrm >> 3) & 7;
    int rm = modrm & 7;

    std::string rmStr;
    if (mod == 3) {
        rmStr = regName(rm, w);
    } else if (mod == 0 && rm == 6) {
        // direct address mode
        rmStr = "[" + std::to_string(read16(code, i)) + "]";
    } else {
        int disp = 0;
        if (mod == 1)
            disp = (int8_t)code[i++];
        if (mod == 2)
            disp = (int16_t)read16(code, i);
        rmStr = std::string("[") + eaBase[rm];
        if (disp > 0)
            rmStr += " + " + std::to_string(disp);
        if (disp < 0)
            rmStr += " - " + std::to_string(-disp);
        rmStr += "]";
    }

    const char *regStr = regName(reg, w);
    if (d)
        println(out, "{} {}, {}", m, regStr, rmStr.c_str());
    else
        println(out, "{} {}, {}", m, rmStr.c_str(), regStr);
}

static void decodeRmImm(const char *m, uint8_t op,
                        const std::vector<uint8_t> &code, size_t &i,
                        std::ofstream &out) {
    bool w = op & 1;
    uint8_t modrm = code[i++];
    int mod = modrm >> 6;
    int rm = modrm & 7;

    std::string rmStr;
    if (mod == 3) {
        rmStr = regName(rm, w);
    } else if (mod == 0 && rm == 6) {
        // direct address mode
        rmStr = "[" + std::to_string(read16(code, i)) + "]";
    } else {
        int disp = 0;
        if (mod == 1)
            disp = (int8_t)read16(code, i);
        if (mod == 2)
            disp = (int16_t)read16(code, i);
        rmStr = std::string("[") + eaBase[rm];
        if (disp > 0)
            rmStr += " + " + std::to_string(disp);
        if (disp < 0)
            rmStr += " - " + std::to_string(-disp);
        rmStr += "]";
    }

    std::string imm = w ? "word " + std::to_string((int16_t)read16(code, i))
                        : "byte " + std::to_string((int8_t)code[i++]);

    println(out, "{} {}, {}", m, rmStr, imm);
}

static void decodeArithRmImm(const char *m, uint8_t op,
                             const std::vector<uint8_t> &code, size_t &i,
                             std::ofstream &out) {
    bool w = op & 1;
    bool s = op & 2;
    uint8_t modrm = code[i++];
    int mod = modrm >> 6;
    int rm = modrm & 7;
    int reg = (modrm >> 3) & 7;

    const char *mnemonic = arithCode[reg];

    std::string rmStr;
    if (mod == 3) {
        rmStr = regName(rm, w);
    } else if (mod == 0 && rm == 6) {
        // direct address mode
        rmStr = "[" + std::to_string(read16(code, i)) + "]";
    } else {
        int disp = 0;
        if (mod == 1)
            disp = (int8_t)read16(code, i);
        if (mod == 2)
            disp = (int16_t)read16(code, i);
        rmStr = std::string("[") + eaBase[rm];
        if (disp > 0)
            rmStr += " + " + std::to_string(disp);
        if (disp < 0)
            rmStr += " - " + std::to_string(-disp);
        rmStr += "]";
    }
    std::string imm;
    if (w && s) {
        imm = "word " + std::to_string((int16_t)code[i++]);
    } else if (w) {
        imm = "word " + std::to_string((int16_t)read16(code, i));
    } else {
        imm = "byte " + std::to_string((int8_t)code[i++]);
    }
    println(out, "{} {}, {}", mnemonic, rmStr, imm);
}

static void decodeMemAcc(const char *m, uint8_t op,
                         const std::vector<uint8_t> &code, size_t &i,
                         std::ofstream &out) {
    bool w = op & 1;
    uint16_t addr = read16(code, i);
    std::string addrStr = "[" + std::to_string(addr) + "]";
    const char *regStr = w ? reg16[0] : reg8[0];

    println(out, "{} {}, {}", m, regStr, addrStr);
}

static void decodeAccMem(const char *m, uint8_t op,
                         const std::vector<uint8_t> &code, size_t &i,
                         std::ofstream &out) {
    bool w = op & 1;
    uint16_t addr = read16(code, i);
    std::string addrStr = "[" + std::to_string(addr) + "]";
    const char *regStr = w ? reg16[0] : reg8[0];

    println(out, "{} {}, {}", m, addrStr, regStr);
}

static void decodeArithImmAcc(const char *m, uint8_t op,
                              const std::vector<uint8_t> &code, size_t &i,
                              std::ofstream &out) {
    bool w = op & 1;
    int arith = (op >> 2) & 7;
    const char *mnemonic = arithCode[arith];

    int imm = w ? (int16_t)read16(code, i) : (int8_t)code[i++];
    const char *regStr = w ? reg16[0] : reg8[0];

    println(out, "{} {}, {}", mnemonic, regStr, imm);
}

struct Pattern {
    uint8_t mask, value;
    OpInfo info;
};

static const Pattern patterns[] = {{0xFC, 0x88, {"mov", decodeModRM}},
                                   {0xFE, 0xC6, {"mov", decodeRmImm}},
                                   {0xF0, 0xB0, {"mov", decodeRegImm}},
                                   {0xFE, 0xA0, {"mov", decodeMemAcc}},
                                   {0xFE, 0xA2, {"mov", decodeAccMem}},
                                   {0xFC, 0x00, {"add", decodeModRM}},
                                   {0xFE, 0x80, {"xxx", decodeArithRmImm}},
                                   {0xFE, 0x04, {"xxx", decodeArithImmAcc}}};

static OpInfo table[256];

static void buildTable() {
    for (int b = 0; b < 256; b++) {
        for (const Pattern &p : patterns) {
            if ((b & p.mask) == p.value) {
                table[b] = p.info;
                break;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::println(stderr, "[8086-decode] Usage: $8086-decode <<bin name>>");
        return 1;
    }

    buildTable();

    // read input bianry file into a vector
    std::string filename = argv[1];
    auto file_size = std::filesystem::file_size(filename);
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::println(stderr, "[8086-decode] Failed to open file");
        return 1;
    }
    std::vector<uint8_t> code(file_size);
    file.read(reinterpret_cast<char *>(code.data()), file_size);

    // crete a file to write to
    std::string out_filename = filename + "_new.asm";
    std::ofstream out(out_filename);
    if (!out) {
        std::println(stderr, "[8086-decode] Failed to open file");
        return 1;
    }

    // loop over binary instructions and write asm to output file
    std::size_t i = 0;
    while (i < code.size()) {
        uint8_t op = code[i++];
        const OpInfo &info = table[op];
        if (!info.decode) {
            std::print(stderr, "db 0x%02X ; unkown\n", op);
            continue;
        }
        info.decode(info.mnemonic, op, code, i, out);
    }

    return 0;
}
