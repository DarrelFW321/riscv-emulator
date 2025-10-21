# 🧠 RISC-V Web Emulator

A lightweight **RISC-V assembly emulator** built with **C++ (Emscripten)** and a modern **HTML/JavaScript frontend** — runs entirely in your browser.  
Created by **Darrel Wihandi** *(Software Engineering @ University of Waterloo)*.

---

## 🚀 Live Demo

🔗 **[Open Emulator on Vercel](https://riscv-emulator.vercel.app/)**  
*(Runs natively in your browser — no installation required.)*

---

## ✨ Features

- **Step-by-step or continuous execution**
  - Inspect registers, memory, and PC updates in real-time.
- **Visual memory inspector**
  - Highlights memory writes (byte, half, word) and detects misalignment.
- **Full RV32I base instruction set coverage**, including:
  - Arithmetic, logic, branches, jumps, loads/stores, immediate ops, and system ECALL.
  - Newly added **unsigned comparisons**, **upper immediate**, and **PC-relative** instructions.

---

## 🧩 Quick Start Example

Paste this into the **Program Input** box:

```asm
ADDI x1, x0, 5
ADDI x2, x0, 10
JAL x5, sum_func
SW x3, 3(x1)
ECALL

sum_func:
  ADD x3, x1, x2
  JALR x0, 0(x5)
