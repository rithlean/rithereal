# rithereal code documentation

These pages explain how the rithereal source code is put together: what each file
does, how the pieces talk to each other, and the main steps inside each one. Every
page has diagrams written in [Mermaid](https://mermaid.js.org/), which GitHub and
VS Code (with the "Markdown Preview Mermaid Support" extension) draw as pictures.

Start with the architecture page, then open the module you want to read about.

| Page | Covers | Source files |
|---|---|---|
| [Architecture](architecture.md) | The big picture: layers, how data flows through each `rith` command, which file depends on which | all |
| [Netlist core](modules/netlist.md) | The gate graph every other part works on | `include/rith/netlist.hpp`, `src/netlist.cpp`, `include/rith/eval.hpp` |
| [Logic simulation](modules/sim.md) | Running 64 test patterns at once, and the equivalence check | `include/rith/sim.hpp`, `src/sim.cpp` |
| [Netlist readers and writers](modules/io.md) | BENCH files, gate-level Verilog in and out, picking a format | `include/rith/io.hpp`, `src/bench.cpp`, `src/verilog.cpp`, `src/file_io.cpp` |
| [Liberty library reader](modules/liberty.md) | Reading cell libraries (`.lib`) and their Boolean functions | `include/rith/liberty.hpp`, `src/liberty.cpp`, `src/libfunc.hpp` |
| [Structural design view](modules/design.md) | Keeping a Verilog netlist cell by cell, and turning it into gates | `include/rith/design.hpp`, `src/design.cpp` |
| [Faults and fault simulation](modules/fault.md) | The stuck-at fault list and the fault simulator | `include/rith/fault.hpp`, `src/fault.cpp` |
| [Testability (SCOAP and COP)](modules/testability.md) | Scores that say how hard each net is to test | `include/rith/testability.hpp`, `src/testability.cpp` |
| [Scan insertion](modules/scan.md) | Turning flops into scan flops and chaining them, plus the DEF/STIL/Tessent files | `include/rith/scan.hpp`, `src/scan.cpp`, `src/scan_out.cpp` |
| [Test point insertion](modules/tpi.md) | Choosing and adding test points | `include/rith/tpi.hpp`, `src/tpi.cpp` |
| [Test generation (ATPG)](modules/atpg.md) | PODEM test generation, redundancy proofs, compaction | `include/rith/atpg.hpp`, `src/atpg.cpp` |
| [The `rith` command](modules/cli.md) | The command-line program that ties it together | `tools/rith_main.cpp` |
| [Tests and CI](modules/tests.md) | How the code is checked | `tests/test_main.cpp`, `tests/data/`, `.github/workflows/ci.yml`, `CMakeLists.txt` |

## How to read the diagrams

- **Block diagrams** (boxes and arrows going one way) show which part uses which.
  An arrow from A to B means "A calls or reads B".
- **Flowcharts** show the steps inside one function, top to bottom. Diamonds are
  yes/no questions; rounded boxes are the start and the end.
- **Sequence diagrams** (in a few places) show the order in which parts talk to each
  other over time.
- Names in `code font` are real names in the source, so you can search for them.

## Words used on these pages

| Word | Plain meaning |
|---|---|
| **DFT** (design for test) | Adding extra hardware to a chip so that, after it is made, a tester can check that it was made correctly. |
| **Netlist** | A list of the gates in a circuit and the wires (nets) that connect them. |
| **Net** | One wire. In rithereal every gate drives exactly one net, and the net has the gate's name. |
| **Gate** | A basic logic block: AND, OR, NOT, XOR, a 2-input multiplexer (MUX), or a flip-flop. |
| **Flip-flop (flop, DFF)** | A one-bit memory. On each clock edge it copies its D input to its Q output. |
| **Cell / library cell** | A gate as the chip factory provides it, such as `NAND2X1` or `SDFFX1`. A Liberty file (`.lib`) describes each cell's pins and logic function. |
| **Mapped netlist** | A netlist made of library cells, as written by Design Compiler, Genus or Yosys with a PDK. |
| **Fan-in / fan-out** | The nets going into a gate / the gates a net goes to. |
| **Levelization** | Numbering gates so that each gate comes after every gate that feeds it. Level 0 is the inputs. |
| **Full-scan view** | Looking at a circuit as if every flop could be loaded and read directly by the tester. Flops then act like extra inputs (their Q) and extra outputs (their D). This turns a sequential circuit into a combinational one, which is much easier to analyse. |
| **Pattern** | One set of 0/1 values put on the inputs (and loaded into the flops) during a test. |
| **Bit-parallel** | Storing 64 patterns in one 64-bit machine word so one CPU instruction simulates 64 patterns at once. |
| **Stuck-at fault** | A model of a manufacturing defect: a net behaves as if it were stuck at 0 (SA0) or stuck at 1 (SA1). |
| **Fault coverage** | The share of faults that a set of patterns can detect. |
| **Fault collapsing** | Merging faults that can never be told apart (for example, an AND input SA0 and the AND output SA0) so each is only simulated once. |
| **Fault simulation** | Simulating the good circuit and the circuit with a fault, and checking whether any output differs. If it does, the pattern detects the fault. |
| **Controllability** | How hard it is to set a net to 0 or to 1 from the inputs. |
| **Observability** | How hard it is to see a change on a net at an output. |
| **SCOAP** | An integer testability score (Goldstein, 1979). Bigger means harder. |
| **COP** | A probability testability score (Brglez, 1984): the chance a net is 1 under random patterns, and the chance a change on it reaches an output. |
| **Scan insertion** | Replacing each flop with a scan flop (a flop with a small multiplexer in front of D) and linking them into shift registers called **scan chains**. With **scan enable** on, the tester can shift values in and out. |
| **Mission mode** | Normal operation of the chip, with all test hardware turned off. |
| **Test point (TP)** | Extra logic added at a hard-to-test net: an **observe point** (a new flop that captures the net) or a **control point** (logic that can force the net to 0 or 1 during test). |
| **TPI** | Test point insertion: choosing where to put test points and adding them. |
| **Clock domain** | All flops clocked by the same clock input on the same edge. |
| **DEF / STIL / Tessent dofile** | Output files for other tools: DEF `SCANCHAINS` for layout tools, STIL for testers and ATPG tools, Tessent setup files for Siemens Tessent. |
| **ATPG** | Automatic test pattern generation: computing patterns aimed at specific faults. See [Test generation](modules/atpg.md). |
