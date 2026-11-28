# The `rith` command

File: `tools/rith_main.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

`rith` is the program people run. It reads the command line, loads the files, calls
the library functions described on the other pages, and prints the results. It holds
no DFT logic of its own.

| Command | Does | Main library calls |
|---|---|---|
| `stats <netlist>` | size, depth, gate mix | `read_netlist_file` |
| `convert <in> <out>` | BENCH to Verilog or back | `read_netlist_file`, `write_netlist_file` |
| `equiv <a> <b> [blocks]` | random equivalence check | `compare_by_simulation` |
| `faultsim <netlist>` | random-pattern stuck-at coverage | `build_fault_list`, `FaultSimulator`, `run_random_patterns` |
| `testability <netlist>` | hardest nets by SCOAP/COP | `compute_scoap`, `compute_cop` |
| `scan <netlist.v> -o out.v` | scan insertion | `insert_scan`, `verify_mission_mode`, `verify_shift`, the writers |
| `tpi <netlist>` | test point selection and insertion | `select_test_points`, `random_coverage`, `insert_test_points` or `apply_test_points` |
| `atpg <netlist>` | stuck-at test generation | `run_atpg`, `write_patterns`, or `Podem::generate` with `--trace` |

## `main()`: dispatch

```mermaid
flowchart TD
  A(["main(argc, argv)"]) --> B{"first argument is --lib?"}
  B -->|yes| C["read_liberty_file and merge_into one Library;<br/>remove the two arguments"] --> B
  B -->|no| D{"library has cells?"}
  D -->|yes| D1["g_lib = the library"]
  D -->|no| E
  D1 --> E{"command and argument count"}
  E -->|"stats, 1 file"| S["stats()"]
  E -->|"convert, 2 files"| CV["convert()"]
  E -->|"equiv, 2 files (+ blocks)"| EQ["equiv()"]
  E -->|faultsim| FS["faultsim()"]
  E -->|testability| TE["testability()"]
  E -->|scan| SC["scan()"]
  E -->|tpi| TP["tpi()"]
  E -->|atpg| AT["atpg()"]
  E -->|anything else| U(["print usage, exit 2"])
  S & CV & EQ & FS & TE & SC & TP & AT --> R(["exit code from the command"])
  E -.->|"any exception"| X(["print 'error: ...', exit 1"])
```

Options after the positional arguments are read with three small helpers:
`option()` (a number), `text_option()` (a string) and `flag()` (present or not).
`load()` reads a netlist and prints any reader warnings to stderr.

## `faultsim`

```mermaid
flowchart TD
  A(["faultsim"]) --> B["load netlist; read --patterns (10000), --seed (1), --show-undetected (0)"]
  B --> C["build_fault_list (collapsed)"]
  C --> D["FaultSimulator, run_random_patterns"]
  D --> E["print faults, patterns (and whether it stopped early),<br/>detected, coverage (collapsed and uncollapsed), time"]
  E --> F["list up to N undetected faults with describe()"]
```

## `testability`

```mermaid
flowchart TD
  A(["testability"]) --> B["load netlist; --top (20)"]
  B --> C["compute_scoap and compute_cop"]
  C --> D["for each net that is not a constant"]
  D --> E{"SCOAP co impossible?"}
  E -->|yes| F["count as unobservable"]
  E -->|no| G["rank by min(COP detect SA0, detect SA1)"]
  F & G --> D
  D -->|done| H["sort, print the N hardest with CC0, CC1, CO, P(1), P(obs), P(detect)"]
```

## `scan`

```mermaid
flowchart TD
  A(["scan"]) --> B{"--lib given and -o given?"}
  B -->|no| X["error"]
  B -->|yes| C["ScanConfig from --chains, --max-length, --se, --si-prefix, --so-prefix"]
  C --> D["read_verilog_design; elaborate = before"]
  D --> E["insert_scan (edits the design)"]
  E --> F["print flops scanned, replaced, already scan, left off (with reasons); chains"]
  F --> G{"--no-verify?"}
  G -->|no| H["elaborate = after;<br/>verify_mission_mode and verify_shift;<br/>status 1 if either fails"]
  G -->|yes| I
  H --> I["write_verilog_design to -o"]
  I --> J["if asked: --def, --stil, --tessent (dofile + testproc), --report"]
  J --> Z(["exit status"])
```

## `tpi`

```mermaid
flowchart TD
  A(["tpi"]) --> B{"--lib given and input is .v / .vg / .sv?"}
  B -->|yes| C["Design mode: read Design, elaborate;<br/>allowed = gates whose name is a real net in the Design"]
  B -->|no| D["Netlist mode: load netlist"]
  C & D --> E["TpiConfig: --points (16), --patterns (32768),<br/>--observe-only / --control-only, --cop-only, --seed"]
  E --> F["select_test_points; print each point and the cost after it"]
  F --> G["random_coverage before and after; print both"]
  G --> H{"-o given?"}
  H -->|no| Z(["exit 0"])
  H -->|"yes, Design mode"| I["insert_test_points (with --enable, --clock);<br/>verify_mission_mode with tp_en held at 0;<br/>write_verilog_design"]
  H -->|"yes, Netlist mode"| J["apply_test_points; write_netlist_file"]
  I --> Z2(["exit 0 if mission mode unchanged, else 1"])
  J --> Z
```

## `atpg`

```mermaid
flowchart TD
  A(["atpg"]) --> B["load netlist; print reader warnings"]
  B --> C{"--trace FAULT?"}
  C -->|yes| D["find the fault by its describe() name;<br/>Podem::generate with trace;<br/>print the steps"] --> Z0(["exit 0"])
  C -->|no| E["AtpgConfig: --backtracks (1000), --random (4096),<br/>--seed, --no-compact"]
  E --> F["run_atpg; print faults, detected, redundant, aborted,<br/>coverage, efficiency, patterns, time"]
  F --> G["--show-aborted N lists aborted faults;<br/>-o writes the patterns"]
  G --> Z(["exit 0, or 1 if run_atpg reported an internal error"])
```

In Design mode points are only allowed on nets that exist in the Verilog, not on the
internal gates that elaboration made inside a cell, so they can be inserted with real
cells.
