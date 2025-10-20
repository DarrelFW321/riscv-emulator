# RISC-V Web Emulator

A lightweight **RISC-V assembly emulator** built with **C++ (Emscripten)** and a simple **HTML/JavaScript UI** — runs entirely in the browser.  
Created by **Darrel Wihandi** (SE @ University of Waterloo).

---

## Live Demo

🔗 **[Open the Emulator on Vercel](https://riscv-emulator.vercel.app/)**  
*(Runs directly in your browser — no installation required.)*

---

## Features

- **Interactive step-by-step execution**
  - View registers, PC, and memory state live.
- **Resizable 3-panel layout**
  - Code editor, CPU state, and console all flexible.
- **Syntax highlighting for current instruction**
- **Memory inspection and misalignment warnings**
- **Implements a core subset of the RISC-V ISA:**
  - Arithmetic, logic, branches, jumps, memory, and system ECALL.

---

## Quick Start

Paste this sample program into the emulator:

```asm
ADDI x1, x0, 5
ADDI x2, x0, 10
JAL x5, sum_func
SW x3, 3(x1)
ECALL

sum_func:
  ADD x3, x1, x2
  JALR x0, 0(x5)
