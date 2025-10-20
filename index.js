// --- Global Setup ---
const createRiscvModule = window.createRiscvModule;
let Module = null;
let prevRegs = Array(32).fill(0);
let prevMem = [];
let pcToLine = [];


// --- Console Helpers ---
function addConsoleLine(text, type = "info") {
  const consoleDiv = document.getElementById("consoleOutput");
  const line = document.createElement("div");
  line.textContent = text;
  line.className = type === "error" ? "console-error" : "console-info";
  consoleDiv.appendChild(line);
  consoleDiv.scrollTop = consoleDiv.scrollHeight;

  // Live instruction display
  if (text.startsWith("[Exec]")) {
    const currInstr = document.getElementById("currInstr");
    currInstr.textContent = "Current Instruction: " + text.replace("[Exec]", "").trim();
  }

  if (text.includes("[Warning]")) {
    flashMemoryWarning();
  }
}

function clearConsole() {
  document.getElementById("consoleOutput").innerHTML = "";
}

// --- UI Setup ---
function setupUI() {
  const getProgram = () => document.getElementById("programInput").value;

  document.getElementById("loadBtn").onclick = () => {
    clearConsole();
    const src = getProgram();
    Module.jsLoadProgram(src);
    addConsoleLine("📜 Program loaded.", "info");
    prevRegs = Array(32).fill(0);
    refreshUI(true);

    // 🔧 build PC→line map (skip comments and labels)
    const lines = src.split("\n");
    pcToLine = [];
    let pc = 0;
    for (let i = 0; i < lines.length; i++) {
      const trimmed = lines[i].trim();
      if (!trimmed || trimmed.startsWith("#") || trimmed.endsWith(":")) continue;
      pcToLine.push({ pc, line: i });
      pc += 4;
    }
  };

  document.getElementById("stepBtn").onclick = () => {
    const ok = Module.jsStep();
    refreshUI();
    if (!ok) addConsoleLine("🛑 Program halted.", "error");
  };

  document.getElementById("runBtn").onclick = runSlow;

  document.getElementById("resetBtn").onclick = () => {
    clearConsole();
    const src = getProgram();
    Module.jsLoadProgram(src);
    addConsoleLine("🔁 CPU Reset & Program Reloaded.", "info");
    prevRegs = Array(32).fill(0);
    refreshUI(true);
    document.getElementById("currInstr").textContent = "Current Instruction: (none)";

    // rebuild mapping
    const lines = src.split("\n");
    pcToLine = [];
    let pc = 0;
    for (let i = 0; i < lines.length; i++) {
      const trimmed = lines[i].trim();
      if (!trimmed || trimmed.startsWith("#") || trimmed.endsWith(":")) continue;
      pcToLine.push({ pc, line: i });
      pc += 4;
    }
};

  // --- Memory search (supports 0xHEX or decimal) ---
  document.getElementById("memSearchBtn").onclick = () => {
    const addrStr = document.getElementById("memSearchInput").value.trim();
    let addr;

    if (/^0x[0-9a-fA-F]+$/.test(addrStr)) addr = parseInt(addrStr, 16);
    else if (/^[0-9]+$/.test(addrStr)) addr = parseInt(addrStr, 10);
    else {
      addConsoleLine(`⚠ Invalid address input: "${addrStr}"`, "error");
      return;
    }

    const aligned = addr - (addr % 4); // round down
    const val = Module.jsReadMemory(aligned);
    addConsoleLine(`🔍 Memory[0x${aligned.toString(16)} (${aligned})] = ${val} (requested 0x${addr.toString(16)})`, "info");
  };

  buildRegTable();
  buildMemTable();
}

// --- Run Loop ---
async function runSlow() {
  addConsoleLine("▶ Running program...", "info");
  for (let i = 0; i < 1000; i++) {
    if (!Module.jsStep()) {
      addConsoleLine("🛑 Program halted.", "error");
      break;
    }
    refreshUI();
    await new Promise(r => setTimeout(r, 200));
  }
  addConsoleLine("🏁 Run complete.", "info");
}

// ------------------ UI Refresh ------------------
function refreshUI(reset = false) {
  const stateStr = Module.jsDumpState();
  const lines = stateStr.split("\n");


  // PC
  const pcMatch = lines[0].match(/PC=0x([0-9a-fA-F]+)/);  
  if (pcMatch) {
    const pcVal = parseInt(pcMatch[1], 16);
    document.getElementById("pcDisplay").textContent = `PC: 0x${pcMatch[1]}`;
    highlightCurrentLine(pcVal);
  }

  // Registers
  const regs = [];
  for (const line of lines) {
    const matches = [...line.matchAll(/x(\d+)=\s*(-?\d+)/g)];
    for (const m of matches) regs[parseInt(m[1])] = parseInt(m[2]);
  }

  const regTable = document.getElementById("regTable");
  for (let i = 0; i < 32; i++) {
    const cell = regTable.rows[Math.floor(i / 4)].cells[i % 4];
    const changed = regs[i] !== prevRegs[i];
    cell.textContent = `x${i.toString().padStart(2, "0")}: ${regs[i]}`;
    cell.className = changed ? "changed" : "";
    prevRegs[i] = regs[i];
  }

  // Memory
  const memLine = lines.find(l => l.includes("Memory[0..63]")); // match new label
  if (memLine) {
    const vals = memLine.split(":")[1].trim().split(/\s+/).map(Number);
    updateMemTable(vals, reset);
  }
}

function buildRegTable() {
  const tbl = document.getElementById("regTable");
  tbl.innerHTML = "";
  for (let r = 0; r < 8; r++) {
    const row = tbl.insertRow();
    for (let c = 0; c < 4; c++) {
      const idx = r * 4 + c;
      const cell = row.insertCell();
      cell.textContent = `x${idx.toString().padStart(2, "0")}: 0`;
    }
  }
}

// ✅ Memory Table: scrollable, hex + decimal display
function buildMemTable() {
  const tbl = document.getElementById("memTable");
  tbl.innerHTML = "";
  const header = tbl.insertRow();
  header.innerHTML = "<th>Address (Hex / Dec)</th><th>Value</th>";

  const ROWS = 64; // must match C++ dump
  for (let i = 0; i < ROWS; i++) {
    const row = tbl.insertRow();
    const addrCell = row.insertCell();
    const valCell = row.insertCell();
    const addr = i * 4;
    addrCell.textContent = `0x${addr.toString(16).padStart(3, "0")} (${addr})`;
    valCell.textContent = "0";
  }
}

// ✅ Green highlight + smooth transition
function updateMemTable(values, reset) {
  const tbl = document.getElementById("memTable");
  const totalRows = tbl.rows.length - 1; // minus header
  const limit = Math.min(values.length, totalRows);

  // (Re)initialize baseline on reset or length change
  if (reset || prevMem.length !== values.length) {
    prevMem = values.slice();
    for (let i = 1; i <= totalRows; i++) {
      const row = tbl.rows[i];
      if (i <= limit) {
        row.cells[1].textContent = values[i - 1];
      } else {
        row.cells[1].textContent = "—";
      }
      row.cells[1].className = "";
    }
    return;
  }

  // Update only rows we have values for
  for (let i = 1; i <= limit; i++) {
    const valCell = tbl.rows[i].cells[1];
    const newVal = values[i - 1];
    const changed = newVal !== prevMem[i - 1];
    valCell.textContent = newVal;
    valCell.className = changed ? "changed" : "";
    prevMem[i - 1] = newVal;
  }

  // For any extra rows in the table, show static placeholder, no highlight
  for (let i = limit + 1; i <= totalRows; i++) {
    const valCell = tbl.rows[i].cells[1];
    if (valCell.textContent !== "—") {
      valCell.textContent = "—";
      valCell.className = "";
    }
  }
}



// --- Panel resizing (horizontal) ---
function setupResizablePanels() {
  const left = document.getElementById("panel-left");
  const middle = document.getElementById("panel-middle");
  const right = document.getElementById("panel-right");
  const divLeft = document.getElementById("divider-left");
  const divRight = document.getElementById("divider-right");

  let activeDivider = null;

  const startResize = (e, divider) => {
    activeDivider = divider;
    document.body.style.cursor = "col-resize";
    e.preventDefault();
  };

  const stopResize = () => {
    activeDivider = null;
    document.body.style.cursor = "default";
  };

  const doResize = (e) => {
    if (!activeDivider) return;
    const totalWidth = document.querySelector(".main").offsetWidth;
    if (activeDivider.id === "divider-left") {
      const newLeft = (e.clientX / totalWidth) * 100;
      left.style.flex = `0 0 ${newLeft}%`;
    } else if (activeDivider.id === "divider-right") {
      const rect = middle.getBoundingClientRect();
      const newMid = ((e.clientX - rect.left) / totalWidth) * 100;
      middle.style.flex = `0 0 ${newMid}%`;
    }
  };

  divLeft.addEventListener("mousedown", e => startResize(e, divLeft));
  divRight.addEventListener("mousedown", e => startResize(e, divRight));
  window.addEventListener("mousemove", doResize);
  window.addEventListener("mouseup", stopResize);
}

// --- Vertical resize (console area) ---
function setupVerticalResize() {
  const divider = document.getElementById("divider-bottom");
  const consoleOut = document.getElementById("consoleOutput");
  const rightPanel = document.getElementById("panel-right");
  let isDragging = false;

  divider.addEventListener("mousedown", (e) => {
    isDragging = true;
    document.body.style.cursor = "row-resize";
    e.preventDefault();
  });

  window.addEventListener("mousemove", (e) => {
    if (!isDragging) return;
    const rect = rightPanel.getBoundingClientRect();
    const offsetY = e.clientY - rect.top;
    const newHeightRatio = offsetY / rect.height;
    const min = 0.2, max = 0.9;
    const clamped = Math.min(Math.max(newHeightRatio, min), max);
    consoleOut.style.flex = `${clamped}`;
  });

  window.addEventListener("mouseup", () => {
    isDragging = false;
    document.body.style.cursor = "default";
  });
}

// --- Initialize after module loads ---
createRiscvModule({
  print: (msg) => addConsoleLine(msg, "info"),
  printErr: (msg) => addConsoleLine(msg, "error"),
}).then(mod => {
  Module = mod;
  addConsoleLine("✅ RISC-V module loaded.", "info");
  setupUI();
  setupResizablePanels();  // horizontal
  setupVerticalResize();   // vertical
  refreshUI(true);
});


function highlightCurrentLine(pc) {
  const ta = document.getElementById("programInput");
  const lines = ta.value.split("\n");
  const match = pcToLine.find(x => x.pc === pc);
  if (!match) return;

  const lineIndex = match.line;

  // compute start and end of that line
  let start = 0;
  for (let i = 0; i < lineIndex; i++) start += lines[i].length + 1;
  const end = start + lines[lineIndex].length;

  // apply selection
  ta.focus();
  ta.setSelectionRange(start, end);

  // scroll into view
  const lineHeight = 18; // approximate per-line height
  ta.scrollTop = lineIndex * lineHeight - ta.clientHeight / 3;
}