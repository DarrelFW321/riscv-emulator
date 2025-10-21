// --- Global Setup ---
const createRiscvModule = window.createRiscvModule;
let Module = null;
let memView = null;  
let prevRegs = Array(32).fill(0);
let prevMem = [];
let lastInspectedAddr = null;
let prevMemBytes = new Map();
let stopRequested = false;
let isRunning = false;
let currentRunId = 0;

const ABI_REG_NAMES = [
  "zero", "ra", "sp", "gp", "tp",  // 0–4
  "t0", "t1", "t2", "s0", "s1", // 5–9
  "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", // 10–17
  "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", // 18–27
  "t3", "t4", "t5", "t6" // 28–31
];
let showAbiNames = false; // toggle flag


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
    stopRequested = true;
    currentRunId++;
    isRunning = false;

    clearConsole();
    const src = getProgram();
    Module.jsLoadProgram(src);

    try { rebindMemView(); } catch (e) { console.error("rebind after load failed:", e); }

    addConsoleLine("📜 Program loaded.", "info");
    prevRegs = Array(32).fill(0);
    refreshUI(true);
  };

  document.getElementById("stepBtn").onclick = () => {
    const ok = Module.jsStep();
    refreshUI();
    if (!ok) addConsoleLine("🛑 Program halted.", "error");
  };

  document.getElementById("runBtn").onclick = runSlow;

  document.getElementById("stopBtn").onclick = () => {
    stopRequested = true;
    addConsoleLine("⏹️ Stop requested.", "info");
      const ta = document.getElementById("programInput");
    // Ensure the selection highlight remains visible
    ta.focus();
  };

  document.getElementById("resetBtn").onclick = async () => {
    stopRequested = true;
    currentRunId++;           // cancels any in-flight runSlow loop
    isRunning = false;
    await Promise.resolve(); 


    clearConsole();
    addConsoleLine("🔄 Reloading RISC-V runtime...", "info");

    // Recreate a fresh module to restore console streams
    Module = await createRiscvModule({
      print: (msg) => addConsoleLine(msg, "info"),
      printErr: (msg) => addConsoleLine(msg, "error"),
    });

    // --- Reload program ---
    const src = document.getElementById("programInput").value;
    Module.jsLoadProgram(src);

    // --- Setup shared memory view ---
    try {
      rebindMemView();    // ⬅️ rebuild the shared view
    } catch (e) {
      console.error("Memory view setup failed during reset:", e);
    }


    addConsoleLine("🔁 CPU Reset & Program Reloaded.", "info");

    // --- Reset register tracking properly ---
    prevRegs = Array(32).fill(0);
    refreshUI(true);

    // After first UI refresh, sync current register snapshot (so SP/GP won't flash)
    const stateStr = Module.jsDumpState();
    const lines = stateStr.split("\n");
    for (const line of lines) {
      const matches = [...line.matchAll(/x(\d+)=\s*(-?\d+)/g)];
      for (const m of matches) prevRegs[parseInt(m[1])] = parseInt(m[2]);
    }
    document.getElementById("currInstr").textContent = "Current Instruction: (none)";
  };


  // --- Memory search (supports 0xHEX or decimal) ---
  document.getElementById("memSearchBtn").onclick = () => {

    if (!memView) {
      try { rebindMemView(); } catch (e) { addConsoleLine("Memory not available", "error"); return; }
    }
    const addrStr = document.getElementById("memSearchInput").value.trim();
    let addr;

    if (/^0x[0-9a-fA-F]+$/.test(addrStr)) addr = parseInt(addrStr, 16);
    else if (/^[0-9]+$/.test(addrStr)) addr = parseInt(addrStr, 10);
    else {
      addConsoleLine(`⚠ Invalid address input: "${addrStr}"`, "error");
      return;
    }

    // Align down to word boundary
    const aligned = addr - (addr % 4);
    const localAddr = aligned; // offset within emulator memory (0..4095)
    const dv = new DataView(memView.buffer, memView.byteOffset + localAddr, 4);
    const val = dv.getUint32(0, true); // little-endian

    addConsoleLine( 
      `🔍 Memory[0x${aligned.toString(16)} (${aligned})] = ${val} (requested 0x${addr.toString(16)})`,
      "info"
    );


    // Show byte neighborhood (true byte reads)
    lastInspectedAddr = addr;
    showMemoryNeighborhood(addr);
  };

  buildRegTable();

  document.getElementById("toggleRegNamesBtn").onclick = () => {
    showAbiNames = !showAbiNames;
    const btn = document.getElementById("toggleRegNamesBtn");
    btn.textContent = showAbiNames ? "Show xN" : "Show ABI";
    refreshUI(true); // refresh table labels immediately
  };
  buildMemTable();
}

// --- Memory Management
function rebindMemView() {
  const cpu = Module.getCpuInstance();
  const basePtr = cpu.getMemoryData();   // uintptr_t -> number
  const memSize = cpu.getMemorySize();

  // Make sure HEAPU8 exists (older builds may not export it)
  if (!Module.HEAPU8) {
    // Try to construct it from the wasm memory
    const mem =
      Module.wasmMemory?.buffer ??
      Module.asm?.memory?.buffer; // fallback for different emscripten layouts
    if (!mem) throw new Error("WASM memory buffer is not available yet");
    Module.HEAPU8 = new Uint8Array(mem);
  }

  memView = new Uint8Array(Module.HEAPU8.buffer, basePtr, memSize);
  console.log(`Shared memory view established: ${memSize} bytes @ 0x${basePtr.toString(16)}`);
}

function showMemoryNeighborhood(targetAddr) {
  if (!memView) {
    try { rebindMemView(); } catch (e) {
      document.getElementById("memInspectResult").innerHTML = "<em>Memory not available.</em>";
      return;
    }
  }

  const container = document.getElementById("memInspectResult");
  const oldScrollLeft =
    container.querySelector(".mem-inspect-scroll")?.scrollLeft ?? 0; // ✅ remember scroll

  const center = Math.max(0, Math.min(targetAddr | 0, memView.length - 1));
  const bytesAround = 4;

  // Clamp window (±4 bytes)
  let start = center - bytesAround;
  let end = center + bytesAround;
  if (start < 0) {
    end = Math.min(end + (-start), memView.length - 1);
    start = 0;
  } else if (end >= memView.length) {
    const overshoot = end - (memView.length - 1);
    start = Math.max(0, start - overshoot);
    end = memView.length - 1;
  }

  const bytes = [];
  for (let a = start; a <= end; a++) bytes.push({ addr: a, val: memView[a] });

  // --- Build new HTML ---
  let html = `<h4>Inspecting address 0x${center.toString(16)} (${center})</h4>`;
  html += `<div class="mem-inspect-scroll"><table class="mem-inspect-table"><tr>`;

  // Header row (addresses)
  for (const b of bytes)
    html += `<th>0x${b.addr.toString(16).padStart(2, "0")}</th>`;
  html += `</tr><tr>`;

  // Data row (values)
  for (const b of bytes) {
    const highlight = b.addr === center ? " mem-highlight" : "";
    html += `<td id="mem-byte-${b.addr}" class="mem-cell${highlight}">0x${b.val
      .toString(16)
      .padStart(2, "0")}</td>`;
  }

  html += `</tr></table></div>`;

  // Word display (safe)
  const wordBase = center & ~3;
  const localBase = Math.min(Math.max(wordBase, 0), memView.length - 4);
  const dv = new DataView(memView.buffer, memView.byteOffset + localBase, 4);
  const wordVal = dv.getUint32(0, true);
  html += `<p><strong>Word @ 0x${localBase.toString(16)}</strong> = 
    <span style="color:#1e90ff;">0x${wordVal.toString(16).padStart(8, "0")}</span> 
    (<span style="color:#80ff80;">${wordVal}</span>) 
    (little-endian)</p>`;

  // --- Replace HTML ---
  container.innerHTML = html;

  // ✅ Restore previous scroll position
  const scrollDiv = container.querySelector(".mem-inspect-scroll");
  if (scrollDiv) scrollDiv.scrollLeft = oldScrollLeft;

  // --- Flash only changed bytes ---
  requestAnimationFrame(() => {
    for (const b of bytes) {
      const cell = document.getElementById(`mem-byte-${b.addr}`);
      const oldVal = prevMemBytes.get(b.addr);
      if (!cell) continue;

      if (oldVal !== undefined && oldVal !== b.val) {
        cell.classList.add("flash");
        setTimeout(() => cell.classList.remove("flash"), 250);
      }
      prevMemBytes.set(b.addr, b.val);
    }
  });
}

// --- Run Loop ---
async function runSlow() {
  // start a new run
  stopRequested = false;
  isRunning = true;
  const myRunId = ++currentRunId;

  addConsoleLine("▶ Running program...", "info");

  for (let i = 0; i < 1000; i++) {
    // abort if another run started or a stop/reset happened
    if (stopRequested || myRunId !== currentRunId) {
      addConsoleLine("⏹️ Execution stopped.", "error");
      break;
    }

    if (!Module.jsStep()) {
      addConsoleLine("🛑 Program halted.", "error");
      break;
    }

    refreshUI();
    await new Promise(r => setTimeout(r, 200));
  }

  if (myRunId === currentRunId && !stopRequested) {
    addConsoleLine("🏁 Run complete.", "info");
  }
  isRunning = false;
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
    const changed = !reset && regs[i] !== prevRegs[i];
    const label = showAbiNames ? ABI_REG_NAMES[i] : `x${i.toString().padStart(2, "0")}`;
    cell.textContent = `${label}: ${regs[i]}`;
    cell.className = changed ? "changed" : "";
    prevRegs[i] = regs[i];
  }

  // Memory
  const memLine = lines.find(l => l.includes("Memory[words 0..63]"));
  if (memLine) {
    // extract pairs like "123(0x7b)"
    const pairs = memLine.split(":")[1].trim().split(/\s+/);
    const values = pairs.map(p => {
      const m = p.match(/(-?\d+)\((0x[0-9a-fA-F]+)\)/);
      if (m) return { dec: parseInt(m[1]), hex: m[2] };
      return { dec: parseInt(p), hex: "0x0" };
    });
    updateMemTable(values, reset);
  }

  if (lastInspectedAddr !== null) {
    showMemoryNeighborhood(lastInspectedAddr);
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
      const label = showAbiNames ? ABI_REG_NAMES[idx] : `x${idx}`;
      cell.textContent = `${label}: 0`;
    }
  }
}

// Memory Table: scrollable, hex + decimal display
function buildMemTable() {
  const tbl = document.getElementById("memTable");
  tbl.innerHTML = "";
  const header = tbl.insertRow();
  header.innerHTML = "<th>Address (Hex / Dec)</th><th>Value (Dec / Hex)</th>";

  const ROWS = 64; // first 64 words = 256 bytes
  for (let i = 0; i < ROWS; i++) {
    const row = tbl.insertRow();
    const addrCell = row.insertCell();
    const valCell = row.insertCell();
    const addr = i * 4; // word-aligned addresses
    addrCell.textContent = `0x${addr.toString(16).padStart(3, "0")} (${addr})`;
    valCell.textContent = "0 (0x0)";
  }
}


// Green highlight + smooth transition
function updateMemTable(values, reset) {
  const tbl = document.getElementById("memTable");
  const totalRows = tbl.rows.length - 1; // minus header
  const limit = Math.min(values.length, totalRows);

  if (reset || prevMem.length !== values.length) {
    prevMem = values.map(v => v.dec);
    for (let i = 1; i <= totalRows; i++) {
      const row = tbl.rows[i];
      if (i <= limit) {
        const v = values[i - 1];
        row.cells[1].textContent = `${v.dec} (${v.hex})`;
      } else {
        row.cells[1].textContent = "—";
      }
      row.cells[1].className = "";
    }
    return;
  }

  for (let i = 1; i <= limit; i++) {
    const valCell = tbl.rows[i].cells[1];
    const v = values[i - 1];
    const changed = v.dec !== prevMem[i - 1];
    valCell.textContent = `${v.dec} (${v.hex})`;
    valCell.className = changed ? "changed" : "";
    prevMem[i - 1] = v.dec;
  }

  for (let i = limit + 1; i <= totalRows; i++) {
    const valCell = tbl.rows[i].cells[1];
    valCell.textContent = "—";
    valCell.className = "";
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

async function initModule() {
  Module = await createRiscvModule({
    print: (msg) => addConsoleLine(msg, "info"),
    printErr: (msg) => addConsoleLine(msg, "error"),
  });

  addConsoleLine("✅ RISC-V module loaded.", "info");

  try {
    rebindMemView();
  } catch (e) {
    console.error("Memory view setup failed:", e);
  }

  setupUI();

  ["stopBtn", "resetBtn", "runBtn", "stepBtn", "loadBtn"].forEach(id => {
    const btn = document.getElementById(id);
    if (!btn) return;
    // Prevent buttons from stealing focus on click/touch
    btn.addEventListener("mousedown", e => e.preventDefault()); 
  });
  setupResizablePanels();
  setupVerticalResize();
  refreshUI(true);
}

// Initial startup
initModule();



function highlightCurrentLine(pc) {
  if (!Module || !Module.getCpuInstance) return;

  try {
    const cpu = Module.getCpuInstance();
    const lineIndex = cpu.getSourceLineForPC(pc);
    if (lineIndex < 0) return; // no valid mapping

    const ta = document.getElementById("programInput");
    const lines = ta.value.split("\n");

    // Compute character offsets to select correct line
    let start = 0;
    for (let i = 0; i < lineIndex; i++) start += lines[i].length + 1;
    const end = start + (lines[lineIndex]?.length ?? 0);

    const wasFocused = document.activeElement === ta;
    ta.setSelectionRange(start, end);
    if (wasFocused) ta.focus();

    // Smooth scroll so that current line is visible
    const lineHeight = 18;
    ta.scrollTop = lineIndex * lineHeight - ta.clientHeight / 3;
  } catch (err) {
    console.warn("Highlight failed:", err);
  }
}


