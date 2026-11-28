# rithereal

Open-source design-for-test (DFT) tools in C++: scan insertion, ML-guided test point
insertion, fault simulation and ATPG for gate-level netlists from Design Compiler,
Genus or Yosys, with IEEE-standard outputs for Tessent, TestMAX, Modus and OpenROAD.

**Status: Phase 4 (ATPG), second step.** The netlist core, readers and writers, a
multi-threaded stuck-at fault simulator, SCOAP/COP testability analysis, mux-D scan
insertion, test point insertion and PODEM test generation with SOCRATES-style learning
work today. Next: a SAT fallback for the faults that are still aborted, then STIL
patterns. The ML-guided test point selector (Phase 3b) is deferred.

## Build

Needs a C++20 compiler (GCC 11+, Clang 14+) and CMake 3.20+. No other dependencies.

```sh
cmake -S . -B build -G Ninja      # or omit -G Ninja to use make
cmake --build build
ctest --test-dir build --output-on-failure
```

Developer options: `-DRITH_SANITIZE=ON` (address and UB sanitizers), `-DRITH_WERROR=ON`.

## Use

```sh
build/rith stats   tests/data/s27.bench              # size, depth, gate mix
build/rith convert tests/data/c17.bench c17.v        # BENCH <-> Verilog
build/rith equiv   tests/data/yosys_small.v tests/data/small_ref.bench
build/rith --lib tests/data/tiny.lib stats tests/data/dc_style.v   # library-mapped netlist
build/rith faultsim    tests/data/s27.bench --patterns 10000      # random-pattern fault coverage
build/rith testability tests/data/c17.bench --top 10              # hardest nets (SCOAP and COP)
```

`equiv` is a random-simulation check in the full-scan view (flops cut, matched by
name). It finds most real differences fast but is not a formal proof. To compare mission
mode of two scanned netlists, or a scanned and an unscanned one, hold the scan enables off
and leave out the scan ports:

```sh
build/rith --lib cells.lib equiv dc_scan.v rith_scan.v --tie SE=0 --ignore SI --ignore SO     --ignore test_si1 --ignore test_so1
```

### Existing scan chains

```sh
build/rith --lib cells.lib chains dc_scan.v --cells   # chains already in a DC/Genus netlist
```

Traces the chains by simulating shift mode, so it sees through buffers, inverters, lockup
latches and DC's non-scan shift-register segments. On 19 DC-inserted SAED32 netlists (up to
17,055 scan cells) the chain lengths match TetraMAX's DRC trace exactly. See
[docs/modules/chains.md](docs/modules/chains.md).

### Scan insertion

```sh
build/rith --lib tests/data/tiny.lib scan tests/data/scan_domains.v -o scanned.v \
    --chains 4 --def scan.def --stil scan.spf --tessent scan --report scan.rpt
```

- Each flop cell is matched to a scan cell from the same library **by Boolean function**,
  not by name: with scan enable off, the scan cell must behave exactly like the original
  (same next state, clock, set/reset and every output the instance uses). Scan pins come
  from the Liberty `signal_type` or common names (SI/SE, SCD/SCE, TI/TE) and are confirmed
  from `next_state`, so active-low scan enables work too. Among matches the closest name
  wins (`DFFX1` becomes `SDFFX1`).
- Flops are grouped into clock domains by the input port that clocks them (traced through
  buffers and inverters) and the active edge, so chains never need lockup latches. Each
  domain is cut into balanced chains; `--chains N` sets the total, `--max-length L` caps
  the length.
- Chains are stitched through the Q outputs (QN when a cell has no Q; inversions are tracked)
  to new ports `test_si<n>`, `test_so<n>` and `test_se`. Flops that already are scan cells
  are restitched. Flops with gated or generated clocks, or with no scan version in the
  library, are left off and listed with the reason.
- The netlist is written back with its original cell names, ports, buses and `assign`s.
- Two checks run by default: **mission mode** (scan enable off, the new design matches the
  old one on every output and flop next state) and **shift** (random data shifted in reaches
  every flop in order and comes out after exactly the chain length). Both are simulations,
  not formal proofs.
- Outputs: DEF `SCANCHAINS` for placement-aware reordering (OpenROAD, Innovus), a STIL 1.0
  protocol, a Tessent dofile and test procedure file, and a text report. The STIL and Tessent
  files are untested against the commercial tools so far.

### Test point insertion

```sh
build/rith tpi design.bench --points 32                 # choose points, report coverage before/after
build/rith --lib cells.lib tpi mapped.v --points 32 -o tp.v   # insert them with library cells
build/rith --lib cells.lib scan tp.v -o scanned.v       # then put the new flops on chains
```

- Three kinds of point: **observe** (a new flop captures the net), **control-0** and
  **control-1** (the net's consumers see it forced to 0 or 1 when the test enable `tp_en`
  and the point's own control flop are both 1). Output ports keep the original net, so
  with `tp_en = 0` the design behaves exactly as before; this is checked by simulation
  after insertion.
- Selection is greedy. Each round, candidates are shortlisted by cheap scores, then each
  is re-scored exactly with a COP pass that includes it. The cost is the expected number of
  faults still undetected after N random patterns, over logic that some output or flop can
  see. Every 4 points a fault simulation re-aims the cost at the nets whose faults really
  escape, since COP alone misjudges reconvergent fanout (`--cop-only` turns this off).
- Coverage is reported with random patterns over the original logic's uncollapsed faults,
  so before and after count the same faults.
- In mapped netlists, control points use AND2/OR2 cells, or NAND2 and INV when the library
  has no AND2/OR2, and the flops are the library's plain D flop.

On ISCAS benchmarks (4 threads), random-pattern stuck-at coverage with 32,768 patterns, at most 32 points (fewer when another
point would not help):

| Circuit | Points | Before | After | Selection time |
|---|---|---|---|---|
| c2670 | 5 | 84.10% | 96.40% | 0.1 s |
| c7552 | 20 | 95.54% | 98.84% | 0.4 s |
| s9234 | 32 | 88.08% | 94.22% | 2.4 s |
| s13207 | 11 | 97.64% | 98.86% | 0.5 s |
| s15850 | 32 | 93.51% | 97.44% | 2.0 s |
| s38417 | 32 | 95.97% | 97.42% | 4.5 s |
| s38584 | 32 | 94.95% | 97.20% | 3.8 s |

### Test generation (ATPG)

```sh
build/rith atpg design.bench -o patterns.txt          # random patterns, then PODEM, then compaction
build/rith atpg design.bench --trace "n12 SA0"        # every PODEM step for one fault
build/rith atpg design.bench --plain                  # plain PODEM, without the SOCRATES additions
```

- Random patterns go first and are kept only when they are the first to detect a fault.
  PODEM (Goel, 1981) then targets each fault still undetected: decisions only on inputs and
  scan cells, three-valued good and faulty machine implication, SCOAP-guided backtrace,
  D-frontier with an X-path check. Each fault ends **detected**, **redundant** (the search
  was exhausted, which proves no test exists) or **aborted** (backtrack limit, default 1000).
- On top of PODEM, three ideas from SOCRATES (Schulz, Trischler and Sarfert, 1988) prune the
  search: **static learning** (contrapositive implications found once per netlist), and
  **static and dynamic unique sensitization** (the side inputs of every gate the fault
  effect must pass through need non-controlling values). These necessary assignments are
  closed under direct and learned implications; a conflict proves the fault redundant
  without searching, or forces an early backtrack.
- Every PODEM pattern is fault-simulated and must detect its target, and the final set is
  compacted by reverse-order fault simulation.
- Correctness is checked against brute force: on every fault of c17, s27 and 150 random
  circuits with every gate type, PODEM with no backtrack limit must say detected exactly
  when some input assignment detects the fault, and its test cube must detect it whatever
  the don't-cares are set to. The choices the paper leaves open are listed in
  `include/rith/atpg.hpp`.
- Some redundancies need more than SOCRATES's implications, mostly around XOR trees: the
  redundant faults of c432, c499 and c1355 stay aborted. A SAT fallback is the next step.

Collapsed faults proved redundant and aborted, plain PODEM against PODEM with the SOCRATES
additions; efficiency is detected plus proved redundant over all (uncollapsed) faults.
Backtrack limit 1000, 4 threads:

| Circuit | Faults | Plain: redundant / aborted | Efficiency | Time | Learning: redundant / aborted | Efficiency | Time |
|---|---|---|---|---|---|---|---|
| c1908 | 1,879 | 6 / 3 | 99.87% | 0.02 s | 9 / 0 | 100.00% | 0.02 s |
| c2670 | 2,747 | 68 / 49 | 98.22% | 0.2 s | 88 / 29 | 98.93% | 0.2 s |
| c3540 | 3,428 | 84 / 53 | 98.57% | 0.5 s | 137 / 0 | 100.00% | 0.06 s |
| c7552 | 7,550 | 62 / 79 | 99.12% | 0.8 s | 77 / 56 | 99.27% | 0.5 s |
| s9234 | 6,927 | 222 / 230 | 96.51% | 2.4 s | 403 / 49 | 99.43% | 1.4 s |
| s38417 | 31,180 | 109 / 62 | 99.91% | 1.0 s | 157 / 9 | 99.98% | 0.9 s |
| s38584 | 36,303 | 1,504 / 2 | 100.00% | 1.5 s | 1,505 / 1 | 100.00% | 1.2 s |
| b17 | 76,491 | 413 / 2,331 | 97.13% | 128 s | 744 / 1,819 | 97.76% | 169 s |
| b19 | 533,276 | 1,526 / 6,525 | 98.87% | 562 s | 2,232 / 3,922 | 99.39% | 696 s |

Over all circuits, aborted faults drop from 212 to 104 on ISCAS'85, 311 to 64 on ISCAS'89
and 12,474 to 8,492 on ITC'99. The largest ITC'99 circuits take longer, mostly in learning.

## What is supported now

| Area | Supported | Not yet |
| --- | --- | --- |
| BENCH | ISCAS'85/'89 style, plus `MUX`, `CONST0`, `CONST1` | |
| Verilog in | One flat module; gate primitives; Yosys generic cells (`$_AND_`, `$_MUX_`, `$_DFF_P_`, ...); Liberty-mapped cells from DC, Genus or a PDK (`--lib`), including scan flops, multi-output cells and either clock edge; `assign` of net, `~net` or constant; bus bits; escaped names; ANSI and non-ANSI headers; Yosys attributes skipped | Hierarchy, concatenations, functional latches (Liberty latches are modelled as transparent, as for lockup latches, with a warning), integrated clock gates; asynchronous set/reset is assumed inactive (with a warning) |
| Verilog out | Flat structural Verilog with the original library cells, ports, buses and `assign`s (`rith scan`, `Design`); or generic primitives with `rith_dff` and `rith_mux2` (`convert`) | Hierarchy |
| Core | Flat gate graph, CSR fanin/fanout, levelization, loop and driver checks | |
| Simulation | 64-pattern bit-parallel, full-scan view | |
| Faults | Single stuck-at on stems and fanout branches; equivalence collapsing; faults in unobservable logic counted separately | Transition faults, dominance collapsing |
| Fault simulation | Parallel-pattern single-fault propagation, event-driven, fault dropping, multi-threaded; checked against brute force | |
| ATPG | Stuck-at, full scan: random phase, PODEM with SCOAP backtrace and X-path check, SOCRATES static learning and unique sensitization, redundancy proofs, reverse-order compaction, plain-text patterns, decision trace | SAT fallback, FAN multiple backtrace, STIL patterns, transition faults, sequential ATPG |
| Testability | SCOAP CC0/CC1/CO and COP probability/observability, full-scan view | |
| Test points | Observe, control-0, control-1; greedy COP selection re-aimed by fault simulation; insertion with library cells, mission-mode check | ML-guided selection (Phase 3b), timing-aware placement of points |
| Scan insertion | Mux-D scan on library cells; function-based scan cell matching; clock-domain and edge grouping; balanced chains; restitching existing scan cells; DEF SCANCHAINS, STIL, Tessent outputs; mission-mode and shift checks | Lockup latches for mixed-clock chains, test-mode control of gated clocks and async set/reset, LSSD and clocked scan, compression |

A 500k-gate netlist parses and levelizes in under a second on one core. Fault simulation of
a 50k-gate random circuit (133k collapsed faults, 4,096 patterns) takes about 6 s on 4 cores. The Sky130 HD library (428 cells, 12.8 MB) parses in 0.15 s, and 404 of its cells map cleanly; the rest are latches and clock gates. All 21 of its non-scan flops (set, reset, enable, either edge) find their scan version by function, for example `dfxtp_2` to `sdfxtp_2` and `edfxtp_1` to `sedfxtp_1`.

## Layout

How the code works, with block diagrams and a flowchart per module: [docs/](docs/README.md).

```
include/rith/   public headers (netlist, eval, sim, io, design, liberty, fault, testability, scan, tpi, atpg)
src/            library sources
tools/          the `rith` command
tests/          tests and test netlists (ISCAS c17, s27, Yosys-style sample)
```

## Roadmap

1. **Phase 0, foundations (done):** netlist core, BENCH/Verilog I/O, Liberty cell mapping for DC/Genus netlists.
2. **Phase 1 (done):** stuck-at fault list, bit-parallel fault simulator, SCOAP/COP.
3. **Phase 2 (done):** scan insertion and stitching; DEF `SCANCHAINS`, STIL and Tessent outputs.
4. **Phase 3:** test point insertion. 3a (done): the classic COP and fault-sim guided baseline.
   3b (deferred): a graph neural network trained on fault-simulation labels, measured against 3a.
5. **Phase 4 (now):** ATPG. PODEM (done), SOCRATES learning (done), then SAT fallback, STIL patterns,
   transition faults. Release v1.0.

## License

Apache-2.0 (see `SPDX-License-Identifier` headers; add the full `LICENSE` text when the repository is created).
