#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <emscripten/bind.h>
using namespace std;
using namespace emscripten;

//-------------------------------------
// Instruction representation
//-------------------------------------
struct Instruction
{
    string op;
    vector<string> args;
    int sourceLine = -1;
};

// ------------------------------------------
// ABI Register Name Map
// ------------------------------------------
static const unordered_map<string, int> ABI_REG_MAP = {
    // Zero & return
    {"zero", 0},
    {"ra", 1},
    {"sp", 2},
    {"gp", 3},
    {"tp", 4},

    // Temporaries
    {"t0", 5},
    {"t1", 6},
    {"t2", 7},
    {"t3", 28},
    {"t4", 29},
    {"t5", 30},
    {"t6", 31},

    // Saved registers
    {"s0", 8},
    {"s1", 9},
    {"s2", 18},
    {"s3", 19},
    {"s4", 20},
    {"s5", 21},
    {"s6", 22},
    {"s7", 23},
    {"s8", 24},
    {"s9", 25},
    {"s10", 26},
    {"s11", 27},

    // Arguments / return values
    {"a0", 10},
    {"a1", 11},
    {"a2", 12},
    {"a3", 13},
    {"a4", 14},
    {"a5", 15},
    {"a6", 16},
    {"a7", 17}};

//-------------------------------------
// RISC-V Emulator core
//-------------------------------------
class SimpleRISCV
{
public:
    vector<int> reg;
    vector<uint8_t> memory; // byte-addressable memory (e.g., 4 KiB)
    unordered_map<string, int> labels;
    vector<Instruction> program;
    int pc = 0;

    SimpleRISCV()
    {
        reg.assign(32, 0);
        memory.assign(4096, 0);
        // Initialize stack pointer (x2 = sp) to end of memory
        reg[2] = memory.size();     // top of 4 KB stack region
        reg[3] = memory.size() / 2; // gp
    }

    //---------------------------------
    // Program loading and parsing
    //---------------------------------
    void loadProgram(const vector<string> &lines)
    {
        program.clear();
        labels.clear();
        pc = 0;

        for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            string rawLine = lines[lineIndex];
            string line = trim(rawLine);

            if (line.empty() || line[0] == '#')
                continue;
            size_t commentPos = line.find('#');
            if (commentPos != string::npos)
                line = trim(line.substr(0, commentPos));
            if (line.empty())
                continue;

            // --- Handle labels ---
            while (true)
            {
                size_t pos = line.find(':');
                if (pos == string::npos)
                    break;
                string label = trim(line.substr(0, pos));
                if (!label.empty())
                    labels[label] = program.size() * 4;
                line = (pos + 1 < line.size()) ? line.substr(pos + 1) : "";
                line = trim(line);
            }
            if (line.empty())
                continue;

            // --- Parse instruction ---
            stringstream ss(line);
            Instruction inst;
            ss >> inst.op;
            inst.sourceLine = (int)lineIndex; // 🟢 assign the source line here

            // normalize
            for (auto &ch : inst.op)
                ch = toupper(ch);

            string argsPart;
            getline(ss, argsPart);
            argsPart = trim(argsPart);
            for (char &ch : argsPart)
                if (ch == ',')
                    ch = ' ';
            stringstream as(argsPart);
            string arg;
            while (as >> arg)
                inst.args.push_back(arg);

            // expand pseudo-instructions
            auto seq = expandPseudo(inst);

            // 🟢 propagate the source line number
            for (auto &e : seq)
            {
                e.sourceLine = inst.sourceLine;
                program.push_back(e);
            }
        }

        cerr << "[RISC-V] Program loaded: " << program.size()
             << " instructions, " << labels.size() << " labels.\n";
    }

    //---------------------------------
    // Step execution
    //---------------------------------
    bool step()
    {
        // enforce x0 = 0
        reg[0] = 0;

        int index = pc / 4;
        if (index < 0 || index >= (int)program.size())
        {
            cerr << "[RISC-V] PC out of range — halting.\n";
            return false;
        }

        Instruction &inst = program[index];
        string op = inst.op;

        // Log every instruction executed
        cerr << "[Exec] " << toString(inst) << " (PC=" << pc << ", Line=" << inst.sourceLine << ")\n";

        if (op == "LA")
        {
            int rd = regNum(inst.args[0]);
            string label = inst.args[1];

            if (!labels.count(label))
            {
                cerr << "[Warning] LA label not found: " << label << "\n";
                pc += 4;
                return true;
            }

            int addr = labels[label];
            int upper = (addr + 0x800) >> 12;
            int lower = addr & 0xFFF;
            if (lower & 0x800)
                lower -= 0x1000; // sign-fix

            writeReg(rd, (upper << 12) + lower); // emulate LUI+ADDI
            pc += 4;
            return true;
        }
        // -------- Arithmetic / Logic --------
        else if (op == "ADD")
            alu3(inst, [](int a, int b)
                 { return a + b; });
        else if (op == "ADDI")
            aluI(inst, [](int a, int b)
                 { return a + b; });
        else if (op == "SUB")
            alu3(inst, [](int a, int b)
                 { return a - b; });
        else if (op == "MUL")
            alu3(inst, [](int a, int b)
                 { return a * b; });
        else if (op == "DIV")
            alu3(inst, [](int a, int b)
                 { return b ? a / b : 0; });
        else if (op == "REM")
            alu3(inst, [](int a, int b)
                 { return b ? a % b : 0; });
        else if (op == "AND")
            alu3(inst, [](int a, int b)
                 { return a & b; });
        else if (op == "OR")
            alu3(inst, [](int a, int b)
                 { return a | b; });
        else if (op == "XOR")
            alu3(inst, [](int a, int b)
                 { return a ^ b; });
        else if (op == "SLL")
            alu3(inst, [](int a, int b)
                 { return a << (b & 0x1F); });
        else if (op == "SRL")
            alu3(inst, [](int a, int b)
                 { return (unsigned)a >> (b & 0x1F); });
        else if (op == "SRA")
            alu3(inst, [](int a, int b)
                 { return a >> (b & 0x1F); });
        else if (op == "SLT")
            alu3(inst, [](int a, int b)
                 { return a < b ? 1 : 0; });
        else if (op == "SLLI")
            aluI(inst, [](int a, int shamt)
                 { return a << (shamt & 0x1F); });
        else if (op == "SRLI")
            aluI(inst, [](int a, int shamt)
                 { return (unsigned)a >> (shamt & 0x1F); });
        else if (op == "SRAI")
            aluI(inst, [](int a, int shamt)
                 { return a >> (shamt & 0x1F); });
        else if (op == "SLTU")
            alu3(inst, [](unsigned a, unsigned b)
                 { return a < b ? 1 : 0; });
        else if (op == "SLTI")
            aluI(inst, [](int a, int b)
                 { return a < b ? 1 : 0; });
        else if (op == "SLTIU")
            aluI(inst, [](unsigned a, unsigned b)
                 { return a < b ? 1 : 0; });
        else if (op == "XORI")
            aluI(inst, [](int a, int b)
                 { return a ^ b; });
        else if (op == "ORI")
            aluI(inst, [](int a, int b)
                 { return a | b; });
        else if (op == "ANDI")
            aluI(inst, [](int a, int b)
                 { return a & b; });

        // -------- Memory --------
        else if (op == "LB" || op == "LBU" || op == "LH" || op == "LHU" || op == "LW")
        {
            int rd = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            imm = signExtend12(imm);

            int addr = reg[rs1] + imm;

            if (op == "LB")
            {
                // byte load: no alignment
                if (!validAddrByte(addr))
                    return false;
                int v = sext8(load8(addr));
                writeReg(rd, v);
            }
            else if (op == "LBU")
            {
                if (!validAddrByte(addr))
                    return false;
                int v = zext8(load8(addr));
                writeReg(rd, v);
            }
            else if (op == "LH")
            {
                if (!checkAligned(addr, 2, "LH") || !validAddrByte(addr + 1))
                    return false;
                int v = sext16(load16(addr));
                writeReg(rd, v);
            }
            else if (op == "LHU")
            {
                if (!checkAligned(addr, 2, "LHU") || !validAddrByte(addr + 1))
                    return false;
                int v = zext16(load16(addr));
                writeReg(rd, v);
            }
            else
            { // LW
                if (!checkAligned(addr, 4, "LW") || !validAddrByte(addr + 3))
                    return false;
                int v = (int)load32(addr);
                writeReg(rd, v);
            }
        }
        else if (op == "SB" || op == "SH" || op == "SW")
        {
            int rs2 = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            imm = signExtend12(imm);

            int addr = reg[rs1] + imm;

            if (op == "SB")
            {
                if (!validAddrByte(addr))
                    return false;
                store8(addr, (uint8_t)(reg[rs2] & 0xFF));
            }
            else if (op == "SH")
            {
                if (!checkAligned(addr, 2, "SH") || !validAddrByte(addr + 1))
                    return false;
                store16(addr, (uint16_t)(reg[rs2] & 0xFFFF));
            }
            else
            { // SW
                if (!checkAligned(addr, 4, "SW") || !validAddrByte(addr + 3))
                    return false;
                store32(addr, (uint32_t)reg[rs2]);
            }
        }

        // -------- Branch / Jump --------
        else if (op == "BEQ" || op == "BNE" || op == "BLT" || op == "BGE" || op == "BLTU" || op == "BGEU")
        {
            int rs1 = regNum(inst.args[0]);
            int rs2 = regNum(inst.args[1]);
            string t = inst.args[2];

            // compute byte offset (label or immediate)
            int offset = (labels.count(t) ? labels[t] - pc : signExtend12(parseNumber(t)));

            bool take = false;

            if (op == "BEQ")
                take = (reg[rs1] == reg[rs2]);
            else if (op == "BNE")
                take = (reg[rs1] != reg[rs2]);
            else if (op == "BLT")
                take = (reg[rs1] < reg[rs2]);
            else if (op == "BGE")
                take = (reg[rs1] >= reg[rs2]);
            else if (op == "BLTU")
                take = ((unsigned)reg[rs1] < (unsigned)reg[rs2]);
            else if (op == "BGEU")
                take = ((unsigned)reg[rs1] >= (unsigned)reg[rs2]);

            if (take)
            {
                pc += offset;
                cerr << "[RISC-V] " << op << " taken → PC=" << pc << "\n";
                return true;
            }
            else
            {
                cerr << "[RISC-V] " << op << " not taken → next PC=" << pc + 4 << "\n";
            }
        }

        else if (op == "JAL")
        {
            int rd = regNum(inst.args[0]);
            string label = inst.args[1];
            writeReg(rd, pc + 4);

            if (labels.count(label))
            {
                pc = labels[label];
            }
            else
            {
                // Allow numeric immediate jumps (e.g., offsets)
                try
                {
                    int offset = parseNumber(label);
                    pc += offset;
                }
                catch (...)
                {
                    cerr << "[Warning] JAL target not found: " << label << "\n";
                    pc += 4;
                }
            }

            cerr << "[RISC-V] JAL → " << label << " (PC=" << pc << ")\n";
            return true;
        }
        else if (op == "JALR")
        {
            int rd = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            imm = signExtend12(imm);
            writeReg(rd, pc + 4);
            pc = (reg[rs1] + imm) & ~1;
            cerr << "[RISC-V] JALR → addr=" << pc << "\n";
            return true;
        }
        else if (op == "LUI")
        {
            int rd = regNum(inst.args[0]);
            int imm = parseNumber(inst.args[1]);
            writeReg(rd, imm << 12);
        }
        else if (op == "AUIPC")
        {
            int rd = regNum(inst.args[0]);
            int imm = parseNumber(inst.args[1]);
            writeReg(rd, pc + (imm << 12));
        }
        else if (op == "ECALL")
        {
            cerr << "[RISC-V] ECALL — program halted.\n";
            return false;
        }

        // -------- Normal sequential flow --------
        reg[0] = 0;
        pc += 4;
        return true;
    }

    static string toString(const Instruction &inst)
    {
        string s = inst.op;
        if (!inst.args.empty())
        {
            s += " ";
            for (size_t i = 0; i < inst.args.size(); i++)
            {
                s += inst.args[i];
                if (i + 1 < inst.args.size())
                    s += ", ";
            }
        }
        return s;
    }

    //---------------------------------
    // Dump state for GUI
    //---------------------------------
    string dumpState() const
    {
        stringstream ss;
        ss << "PC=0x" << hex << pc << dec << "\n";
        for (int i = 0; i < 32; i++)
        {
            ss << "x" << setfill('0') << setw(2) << i << setfill(' ')
               << "=" << setw(11) << reg[i]
               << ((i + 1) % 8 == 0 ? "\n" : "  ");
        }

        // Show first 64 words (256 bytes), reconstructed little-endian
        ss << "\nMemory[words 0..63]: ";
        int maxWords = min(64, (int)memory.size() / 4);
        for (int w = 0; w < maxWords; ++w)
        {
            int addr = w * 4;
            uint32_t val = (uint32_t)(memory[addr] |
                                      (memory[addr + 1] << 8) |
                                      (memory[addr + 2] << 16) |
                                      (memory[addr + 3] << 24));
            ss << dec << val << "(" << hex << showbase << val << noshowbase << dec << ") ";
        }
        ss << "\n";
        return ss.str();
    }

    //---------------------------------
    // Read memory (for search)
    //---------------------------------
    uint8_t *getMemoryData() { return memory.data(); }
    size_t getMemorySize() const { return memory.size(); }

    // For Line Highlights
    int getSourceLineForPC(int pcValue) const
    {
        int idx = pcValue / 4;
        if (idx < 0 || idx >= (int)program.size())
            return -1;
        return program[idx].sourceLine;
    }

private:
    //---------------------------------
    // Helpers
    //---------------------------------
    void writeReg(int rd, int val)
    {
        if (rd != 0)
            reg[rd] = val;
    }

    template <typename F>
    void alu3(const Instruction &ins, F fn)
    {
        int rd = regNum(ins.args[0]);
        int rs1 = regNum(ins.args[1]);
        int rs2 = regNum(ins.args[2]);
        writeReg(rd, fn(reg[rs1], reg[rs2]));
    }

    template <typename F>
    void aluI(const Instruction &ins, F fn)
    {
        int rd = regNum(ins.args[0]);
        int rs1 = regNum(ins.args[1]);
        int imm = signExtend12(parseNumber(ins.args[2]));
        writeReg(rd, fn(reg[rs1], imm));
    }

    pair<int, int> parseMem(const string &s) const
    {
        size_t lparen = s.find('(');
        size_t rparen = s.find(')');
        if (lparen == string::npos || rparen == string::npos)
            throw runtime_error("Invalid memory syntax: " + s);

        string immStr = s.substr(0, lparen);
        string rsStr = s.substr(lparen + 1, rparen - lparen - 1);

        int imm = parseNumber(immStr);
        int rs = regNum(rsStr);
        return {imm, rs};
    }

    static int regNum(const string &s)
    {
        string name = s;
        // lowercase everything for consistency
        for (auto &c : name)
            c = tolower(c);

        // Handle xN format
        if (!name.empty() && name[0] == 'x')
        {
            try
            {
                return parseNumber(name.substr(1));
            }
            catch (...)
            {
                cerr << "[Error] Invalid register: " << s << "\n";
                return 0;
            }
        }

        // Handle ABI register name
        auto it = ABI_REG_MAP.find(name);
        if (it != ABI_REG_MAP.end())
            return it->second;

        cerr << "[Warning] Unknown register name: " << s << " → default x0\n";
        return 0;
    }

    static string trim(string s)
    {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == string::npos ? "" : s.substr(a, b - a + 1);
    }

    static int parseNumber(const string &numStr)
    {
        string s = trim(numStr);
        if (s.empty())
            return 0;

        auto to32 = [](long long v) -> int
        {
            uint32_t u = static_cast<uint32_t>(v); // wrap like RV32
            return static_cast<int>(u);            // interpret as signed
        };

        try
        {
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            {
                long long v = stoll(s, nullptr, 16);
                return to32(v);
            }
            if (s.size() > 3 && s[0] == '-' && s[1] == '0' && (s[2] == 'x' || s[2] == 'X'))
            {
                long long v = stoll(s.substr(1), nullptr, 16);
                return to32(-v);
            }
            long long v = stoll(s, nullptr, 10);
            return to32(v);
        }
        catch (...)
        {
            cerr << "[Error] Bad immediate: " << s << "\n";
            return 0;
        }
    }

    static inline int signExtend12(int imm)
    {
        return (imm << 20) >> 20; // keep lower 12 bits, sign-extend
    }

    bool validAddr(int addr) const
    {
        if (addr < 0 || addr >= (int)memory.size() * 4)
        {
            cerr << "[Warning] Memory access out of bounds at address 0x"
                 << hex << addr << dec
                 << " (valid range: 0–" << (memory.size() * 4 - 4) << ")\n";
            return false;
        }
        return true;
    }

    // ---- Address checks ----
    bool validAddrByte(int addr) const
    {
        if (addr < 0 || addr >= (int)memory.size())
        {
            cerr << "[Warning] Memory access OOB at 0x" << hex << addr << dec
                 << " (valid 0.." << (int)memory.size() - 1 << ")\n";
            return false;
        }
        return true;
    }
    bool checkAligned(int addr, int align, const char *what) const
    {
        if (addr % align != 0)
        {
            cerr << "[Warning] Misaligned " << what << " at 0x"
                 << hex << addr << dec << " (align " << align << ")\n";
            return false;
        }
        return true;
    }

    // ---- Little-endian loads ----
    uint8_t load8(int addr) const
    {
        if (!validAddrByte(addr))
            return 0;
        return memory[addr];
    }
    uint16_t load16(int addr) const
    {
        if (!validAddrByte(addr) || !validAddrByte(addr + 1))
            return 0;
        // little-endian
        return (uint16_t)(memory[addr] | (memory[addr + 1] << 8));
    }
    uint32_t load32(int addr) const
    {
        if (!validAddrByte(addr) || !validAddrByte(addr + 3))
            return 0;
        return (uint32_t)(memory[addr] | (memory[addr + 1] << 8) | (memory[addr + 2] << 16) | (memory[addr + 3] << 24));
    }

    // ---- Little-endian stores ----
    void store8(int addr, uint8_t v)
    {
        if (!validAddrByte(addr))
            return;
        memory[addr] = v;
    }
    void store16(int addr, uint16_t v)
    {
        if (!validAddrByte(addr) || !validAddrByte(addr + 1))
            return;
        memory[addr] = (uint8_t)(v & 0xFF);
        memory[addr + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    void store32(int addr, uint32_t v)
    {
        if (!validAddrByte(addr) || !validAddrByte(addr + 3))
            return;
        memory[addr] = (uint8_t)(v & 0xFF);
        memory[addr + 1] = (uint8_t)((v >> 8) & 0xFF);
        memory[addr + 2] = (uint8_t)((v >> 16) & 0xFF);
        memory[addr + 3] = (uint8_t)((v >> 24) & 0xFF);
    }

    // ---- Sign/zero extension helpers ----
    static int sext8(uint8_t v) { return (int)(int8_t)v; }
    static int sext16(uint16_t v) { return (int)(int16_t)v; }
    static int zext8(uint8_t v) { return (int)v; }
    static int zext16(uint16_t v) { return (int)v; }

    //--- Pseudo Helpers
    vector<Instruction> expandPseudo(const Instruction &instIn)
    {
        Instruction inst = instIn;
        string op = inst.op;
        for (auto &c : op)
            c = toupper(c);

        // --- MV rd, rs → ADDI rd, rs, 0 ---
        if (op == "MV" && inst.args.size() == 2)
        {
            Instruction addi{"ADDI", {inst.args[0], inst.args[1], "0"}};
            addi.sourceLine = inst.sourceLine;
            return {addi};
        }

        // --- LI rd, imm ---
        if (op == "LI" && inst.args.size() == 2)
        {
            string rd = inst.args[0];
            int imm = parseNumber(inst.args[1]);

            if (imm >= -2048 && imm <= 2047)
            {
                Instruction addi{"ADDI", {rd, "x0", to_string(imm)}};
                addi.sourceLine = inst.sourceLine;
                return {addi};
            }
            else
            {
                uint32_t uimm = static_cast<uint32_t>(imm);
                int upper = static_cast<int>((uimm + 0x800) >> 12);
                int lower = static_cast<int>(uimm & 0xFFF);
                if (lower & 0x800)
                    lower -= 0x1000;

                Instruction lui{"LUI", {rd, to_string(upper)}};
                Instruction addi{"ADDI", {rd, rd, to_string(lower)}};
                lui.sourceLine = inst.sourceLine;
                addi.sourceLine = inst.sourceLine;
                return {lui, addi};
            }
        }

        // --- LA rd, label ---
        if (op == "LA" && inst.args.size() == 2)
        {
            Instruction la{"LA", {inst.args[0], inst.args[1]}};
            la.sourceLine = inst.sourceLine;
            return {la};
        }

        // --- J label ---
        if (op == "J" && inst.args.size() == 1)
        {
            Instruction jal{"JAL", {"x0", inst.args[0]}};
            jal.sourceLine = inst.sourceLine;
            return {jal};
        }

        // --- JR rs ---
        if (op == "JR" && inst.args.size() == 1)
        {
            Instruction jalr{"JALR", {"x0", "0(" + inst.args[0] + ")"}};
            jalr.sourceLine = inst.sourceLine;
            return {jalr};
        }

        // --- RET ---
        if (op == "RET")
        {
            Instruction jalr{"JALR", {"x0", "0(x1)"}};
            jalr.sourceLine = inst.sourceLine;
            return {jalr};
        }

        // --- Default (unchanged) ---
        Instruction unchanged = instIn;
        unchanged.sourceLine = inst.sourceLine;
        return {unchanged};
    }
};

//-------------------------------------
// Emscripten Bindings
//-------------------------------------
SimpleRISCV cpu;

void jsLoadProgram(string src)
{
    vector<string> lines;
    string line;
    stringstream ss(src);
    while (getline(ss, line))
        lines.push_back(line);
    cpu = SimpleRISCV();
    cpu.loadProgram(lines);
}

bool jsStep() { return cpu.step(); }
string jsDumpState() { return cpu.dumpState(); }

SimpleRISCV *getCpuInstance() { return &cpu; }

EMSCRIPTEN_BINDINGS(riscv_bindings)
{
    emscripten::function("jsLoadProgram", &jsLoadProgram);
    emscripten::function("jsStep", &jsStep);
    emscripten::function("jsDumpState", &jsDumpState);
    emscripten::function("getCpuInstance", &getCpuInstance, emscripten::allow_raw_pointers());

    emscripten::class_<SimpleRISCV>("SimpleRISCV")
        .function("getMemorySize", &SimpleRISCV::getMemorySize)
        .function("getSourceLineForPC", &SimpleRISCV::getSourceLineForPC)
        .function("getMemoryData",
                  emscripten::optional_override([](SimpleRISCV &self)
                                                { return reinterpret_cast<uintptr_t>(self.getMemoryData()); }));
}
