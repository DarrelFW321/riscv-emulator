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
};

//-------------------------------------
// RISC-V Emulator core
//-------------------------------------
class SimpleRISCV
{
public:
    vector<int> reg;
    vector<int> memory; // 1024 words (4 KB)
    unordered_map<string, int> labels;
    vector<Instruction> program;
    int pc = 0;

    SimpleRISCV()
    {
        reg.assign(32, 0);
        memory.assign(1024, 0);
    }

    //---------------------------------
    // Program loading and parsing
    //---------------------------------
    void loadProgram(const vector<string> &lines)
    {
        program.clear();
        labels.clear();
        pc = 0;

        for (auto &raw : lines)
        {
            string line = trim(raw);
            if (line.empty() || line[0] == '#')
                continue;

            // Label definition
            if (line.find(':') != string::npos)
            {
                string label = line.substr(0, line.find(':'));
                labels[label] = program.size() * 4; // byte address
                continue;
            }

            // Parse instruction
            stringstream ss(line);
            Instruction inst;
            ss >> inst.op;
            string arg;
            while (ss >> arg)
            {
                if (arg[0] == '#')
                    break; // stop reading on comment
                if (arg.back() == ',')
                    arg.pop_back();
                inst.args.push_back(arg);
            }
            program.push_back(inst);
        }
        cerr << "[RISC-V] Program loaded: " << program.size() << " instructions.\n";
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
        cerr << "[Exec] " << toString(inst) << " (PC=" << pc << ")\n";

        // -------- Arithmetic / Logic --------
        if (op == "ADD")
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

        // -------- Memory --------
        else if (op == "LW")
        {
            int rd = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            int addr = reg[rs1] + imm;
            if (addr % 4 != 0)
            {
                cerr << "[Warning] Misaligned LW at address 0x" << hex << addr << dec << "\n";
                return false;
            }
            if (!validAddr(addr))
            {
                return false;
            }
            int idx = addr / 4;
            writeReg(rd, memory[idx]);
        }
        else if (op == "SW")
        {
            int rs2 = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            int addr = reg[rs1] + imm;
            if (addr % 4 != 0)
            {
                cerr << "[Warning] Misaligned SW at address 0x" << hex << addr << dec << "\n";
                return false;
            }
            if (!validAddr(addr))
            {
                return false;
            }

            int idx = addr / 4;
            memory[idx] = reg[rs2];
        }

        // -------- Branch / Jump --------
        else if (op == "BEQ" || op == "BNE" || op == "BLT" || op == "BGE" || op == "BLTU" || op == "BGEU")
        {
            int rs1 = regNum(inst.args[0]);
            int rs2 = regNum(inst.args[1]);
            string t = inst.args[2];

            // compute byte offset (label or immediate)
            int offset = (labels.count(t) ? labels[t] - pc : stoi(t));

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
            pc = labels[label];
            cerr << "[RISC-V] JAL → " << label << " (PC=" << pc << ")\n";
            return true;
        }
        else if (op == "JALR")
        {
            int rd = regNum(inst.args[0]);
            auto [imm, rs1] = parseMem(inst.args[1]);
            writeReg(rd, pc + 4);
            pc = (reg[rs1] + imm) & ~1;
            cerr << "[RISC-V] JALR → addr=" << pc << "\n";
            return true;
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
               << "=" << setw(6) << reg[i]
               << ((i + 1) % 8 == 0 ? "\n" : "  ");
        }

        // ✅ Print first 64 *words* (indices 0..63), not skipping
        ss << "\nMemory[0..63]: ";
        for (int i = 0; i < 64 && i < (int)memory.size(); i++)
            ss << memory[i] << " ";
        ss << "\n";
        return ss.str();
    }

    //---------------------------------
    // Read memory (for search)
    //---------------------------------
    int readMemory(int addr) const
    {
        if (addr % 4 != 0)
            return 0;
        int idx = addr / 4;
        if (idx < 0 || idx >= (int)memory.size())
            return 0;
        return memory[idx];
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
        int imm = parseNumber(ins.args[2]);
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
        return s[0] == 'x' ? stoi(s.substr(1)) : stoi(s);
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

        // Handle 0x or 0X prefixes
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
            return stoi(s, nullptr, 16);

        // Handle possible negative numbers
        if (s[0] == '-' && s.size() > 3 && s[1] == '0' && (s[2] == 'x' || s[2] == 'X'))
            return -stoi(s.substr(1), nullptr, 16);

        // Fallback: decimal
        return stoi(s, nullptr, 10);
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
int jsReadMemory(int addr) { return cpu.readMemory(addr); }

EMSCRIPTEN_BINDINGS(riscv_bindings)
{
    emscripten::function("jsLoadProgram", &jsLoadProgram);
    emscripten::function("jsStep", &jsStep);
    emscripten::function("jsDumpState", &jsDumpState);
    emscripten::function("jsReadMemory", &jsReadMemory);
}
