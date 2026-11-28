# Architecture

rithereal is a C++20 library (`rith_core`) plus one command-line program (`rith`)
and one test program (`rith_tests`). It has no outside dependencies apart from the
C++ standard library and threads.

The job of the tool is to take a gate-level netlist (from Yosys, Design Compiler,
Genus or an ISCAS benchmark file), analyse how testable it is, and add test
hardware to it (scan chains and test points), then write the netlist back out in a
form the commercial tools accept.

## Block diagram

The code is in layers. Each layer only uses the layers below it.

```mermaid
flowchart TB
  subgraph APP["Programs"]
    CLI["rith command<br/>tools/rith_main.cpp"]
    TESTS["rith_tests<br/>tests/test_main.cpp"]
  end

  subgraph DFT["DFT passes: change the design"]
    SCAN["Scan insertion<br/>scan.hpp, scan.cpp"]
    SCANOUT["Scan outputs: report, DEF, STIL, Tessent<br/>scan_out.cpp"]
    TPI["Test point insertion<br/>tpi.hpp, tpi.cpp"]
  end

  subgraph ANALYSIS["Analysis: measure the design"]
    FAULT["Fault list and fault simulator<br/>fault.hpp, fault.cpp"]
    TEST["SCOAP and COP testability<br/>testability.hpp, testability.cpp"]
    SIM["Logic simulation and equivalence check<br/>sim.hpp, sim.cpp"]
  end

  subgraph IO["Readers and writers"]
    FILEIO["Pick format by file extension<br/>file_io.cpp"]
    BENCH["BENCH reader and writer<br/>bench.cpp"]
    VLOG["Gate-level Verilog writer<br/>verilog.cpp"]
    DESIGN["Structural design view:<br/>Verilog parser, elaborator, writer<br/>design.hpp, design.cpp"]
    LIB["Liberty .lib reader<br/>liberty.hpp, liberty.cpp"]
    LIBFUNC["Cell function evaluation<br/>libfunc.hpp"]
  end

  subgraph CORE["Core data model"]
    NET["Netlist: gate graph,<br/>fan-in/fan-out, levels<br/>netlist.hpp, netlist.cpp"]
    EVAL["Evaluate one gate on 64 patterns<br/>eval.hpp"]
  end

  CLI --> DFT & ANALYSIS & IO
  TESTS --> DFT & ANALYSIS & IO

  SCAN --> DESIGN & LIB & LIBFUNC & SIM
  SCANOUT --> SCAN
  TPI --> FAULT & DESIGN & LIB & LIBFUNC
  FAULT --> SIM & EVAL
  TEST --> NET
  SIM --> EVAL
  FILEIO --> BENCH & VLOG
  VLOG --> DESIGN
  DESIGN --> LIB & NET
  LIBFUNC --> LIB
  BENCH --> NET
  EVAL --> NET
```

### Two views of one circuit

The most important idea in the code is that a circuit is held in two different ways.

```mermaid
flowchart LR
  V["Verilog file<br/>(cells, pins, buses)"] -->|read_verilog_design| D["Design<br/>cell instances as written"]
  L["Liberty .lib"] -->|read_liberty| LB["Library<br/>cell pins and functions"]
  D -->|elaborate| N["Netlist<br/>primitive gates only"]
  LB -.->|cell functions| N
  B["BENCH file"] -->|read_bench| N
  D -->|write_verilog_design| VO["Verilog file<br/>same cell names"]
  N -->|"write_bench / write_verilog"| NO["BENCH or generic Verilog"]
  SCANP["scan and TPI insertion"] -->|edit| D
  ANA["simulation, faults, SCOAP/COP, TPI selection"] -->|read| N
```

- **`Design`** ([design.md](modules/design.md)) keeps the netlist exactly as the
  synthesis tool wrote it: library cell names, named pin connections, buses and
  `assign` statements. Passes that *change* the circuit (scan insertion, test point
  insertion) edit the `Design`, so the result goes straight back into Design
  Compiler, Genus, Tessent or OpenROAD with the same cell names.
- **`Netlist`** ([netlist.md](modules/netlist.md)) is a simple graph of primitive
  gates (AND, OR, NOT, XOR, MUX, DFF, ...). Every *analysis* (simulation, fault
  simulation, SCOAP/COP, choosing test points) runs on it, because primitive gates
  are fast and easy to reason about.
- **`elaborate()`** turns a `Design` into a `Netlist`. For each library cell it
  looks up the cell's Boolean function in the Liberty library and builds primitive
  gates for it. It also records which flop gate came from which instance, so a
  result found on the `Netlist` can be mapped back to the `Design`.

## How data flows through each command

### `rith stats`, `convert`, `equiv`

```mermaid
flowchart LR
  F["netlist file"] --> R["read_netlist_file"]
  R --> N["Netlist"]
  N --> S["stats: count gates, depth"]
  N --> C["convert: write_netlist_file"]
  N --> E["equiv: compare_by_simulation<br/>against a second Netlist"]
```

### `rith faultsim` and `rith testability`

```mermaid
flowchart LR
  F["netlist file"] --> N["Netlist"]
  N --> FL["build_fault_list<br/>(collapsed)"]
  FL --> FS["FaultSimulator"]
  FS --> RP["run_random_patterns"]
  RP --> OUT1["coverage report"]
  N --> SC["compute_scoap"]
  N --> CP["compute_cop"]
  SC & CP --> OUT2["hardest nets table"]
```

### `rith scan`

```mermaid
flowchart TB
  F["mapped Verilog"] --> RD["read_verilog_design"]
  LIBF[".lib files"] --> RL["read_liberty_file + merge_into"]
  RD --> D["Design"]
  RL --> L["Library"]
  D --> E1["elaborate: Netlist before"]
  D & L --> INS["insert_scan<br/>edits Design in place"]
  INS --> R["ScanResult: chains, excluded flops"]
  INS --> E2["elaborate: Netlist after"]
  E1 & E2 & R --> VM["verify_mission_mode"]
  E2 & R --> VS["verify_shift"]
  INS --> WV["write_verilog_design: scanned.v"]
  R --> W1["write_def_scanchains"]
  R --> W2["write_stil_protocol"]
  R --> W3["write_tessent_dofile + testproc"]
  R --> W4["write_scan_report"]
```

### `rith tpi`

```mermaid
flowchart TB
  F["netlist file"] --> Q{"--lib given and<br/>file is Verilog?"}
  Q -->|yes| DM["Design mode: read Design,<br/>elaborate to Netlist,<br/>allow points only on real nets"]
  Q -->|no| NM["Netlist mode: read Netlist"]
  DM & NM --> SEL["select_test_points<br/>greedy COP + fault-sim re-aim"]
  SEL --> COV["random_coverage before and after"]
  COV --> O{"-o given?"}
  O -->|no| DONE(["print report"])
  O -->|"yes, Design mode"| ID["insert_test_points with library cells<br/>then verify_mission_mode with tp_en = 0"]
  O -->|"yes, Netlist mode"| AP["apply_test_points<br/>then write_netlist_file"]
  ID --> WV["write_verilog_design"]
```

The normal full flow, as the README suggests, chains these commands:

```mermaid
flowchart LR
  SYN["Yosys / DC / Genus<br/>mapped netlist"] --> TP["rith tpi -o tp.v"]
  TP --> SC["rith scan -o scanned.v"]
  SC --> OUTS["scanned.v + DEF + STIL + Tessent files"]
  OUTS --> ATPG["ATPG in Tessent / TestMAX,<br/>or layout in OpenROAD / Innovus"]
```

## Header dependencies

Which public header includes which (from `#include` lines). `libfunc.hpp` is
private to `src/`.

```mermaid
flowchart BT
  netlist["netlist.hpp"]
  eval["eval.hpp"] --> netlist
  sim["sim.hpp"] --> eval & netlist
  liberty["liberty.hpp"]
  io["io.hpp"] --> liberty & netlist
  design["design.hpp"] --> liberty & netlist
  fault["fault.hpp"] --> eval & netlist
  testability["testability.hpp"] --> netlist
  scan["scan.hpp"] --> design & liberty
  tpi["tpi.hpp"] --> design & liberty & netlist
  libfunc["src/libfunc.hpp"] --> liberty
```

## Main data structures

```mermaid
classDiagram
  class Netlist {
    name
    warnings
    gates_ : vector~Gate~
    inputs() outputs() flops()
    fanins(id) fanouts(id)
    topo_order() level(id) depth()
    get_or_declare() define() add_gate()
    finalize()
  }
  class Gate {
    type : GateType
    name
    clock : GateId
    falling_edge
  }
  class Design {
    name
    ports : vector~PortDecl~
    buses
    instances : vector~Instance~
    assigns : vector~Assign~
    net() find_net() fresh_net()
    add_instance() add_assign() add_port()
  }
  class Instance {
    type
    name
    positional
    conns : vector~PinConn~
    net_on(pin) connect(pin, net)
  }
  class Library {
    name
    cells : map~string, LibCell~
    find(cell)
  }
  class LibCell {
    name
    pins : vector~LibPin~
    flop : optional~LibFlop~
    has_latch has_statetable is_scan
  }
  class Elaboration {
    netlist : Netlist
    flop_of_instance : vector~GateId~
  }
  class FaultList {
    faults : vector~Fault~
    weight
    uncollapsed
    unused
  }
  class FaultSimulator {
    simulate_block()
    detect_mask()
    coverage()
  }
  class ScanResult {
    chains : vector~ScanChain~
    se_port
    excluded
    replaced restitched
  }
  class TpiResult {
    points : vector~TestPoint~
    cost : vector~double~
  }
  Netlist "1" *-- "many" Gate
  Design "1" *-- "many" Instance
  Library "1" *-- "many" LibCell
  Elaboration *-- Netlist
  Design ..> Elaboration : elaborate()
  Library ..> Elaboration : cell functions
  FaultSimulator --> Netlist : reads
  FaultSimulator *-- FaultList
  ScanResult ..> Design : describes edits to
  TpiResult ..> Netlist : nets chosen in
```

## Design rules that run through the code

- **Gate IDs, not pointers.** Gates are numbered (`GateId`, a 32-bit integer) and
  every per-gate value (simulation words, SCOAP numbers, COP probabilities) is a
  plain `std::vector` indexed by that number. This is fast and easy to copy.
- **Build, then freeze.** A `Netlist` is built with `define()` calls and then
  `finalize()` packs the connections into compact arrays (CSR) and computes the
  level order. After that it is read-only.
- **Full-scan view everywhere.** Analysis treats each flop's Q as an input and its
  D as an output. This is what scan hardware gives the tester, and it keeps the
  algorithms combinational.
- **Check by simulation.** Every change to a design is checked with random
  simulation: mission mode must be unchanged, scan chains must shift correctly.
  These are strong checks but not formal proofs.
- **Match cells by function, not by name.** Liberty cell functions are evaluated
  by truth table (`libfunc.hpp`), so the tool works with any library (SAED32,
  Sky130, commercial) without a table of cell names.
