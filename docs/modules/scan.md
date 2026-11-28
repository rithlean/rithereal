# Scan insertion

Files: `include/rith/scan.hpp`, `src/scan.cpp`, `src/scan_out.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Scan insertion makes every flop loadable and readable by the tester. Each normal
flop is swapped for a **mux-D scan flop**: the same flop with a small multiplexer in
front of D. When scan enable (`test_se`) is 0 the flop works as before (mission
mode). When it is 1 the flop takes its value from the scan input (SI) instead, and
the flops, linked Q to SI, form a shift register (a **scan chain**).

```mermaid
flowchart LR
  SI["test_si1<br/>(new input)"] --> F1["SDFF 1<br/>SI from test_si1"]
  F1 -->|"Q to SI"| F2["SDFF 2"]
  F2 -->|"Q to SI"| F3["SDFF 3"]
  F3 -->|"Q"| SO["test_so1<br/>(new output)"]
  SE["test_se<br/>(new input)"] -.-> F1 & F2 & F3
  CLK["clock port"] -.-> F1 & F2 & F3
```

`insert_scan()` edits a [`Design`](design.md) in place and returns a `ScanResult`
listing the chains, which flops were left off and why, and which cells were swapped.
Two checks then confirm the result by simulation, and `scan_out.cpp` writes files
for other tools.

## The whole pass

```mermaid
flowchart TD
  A(["insert_scan(design, lib, cfg)"]) --> E["elaborate the design to a Netlist<br/>(to know flops and clocks)"]
  E --> S1["1. for each flop instance: find its scan cell<br/>and its clock domain, or exclude it"]
  S1 --> Q{"any usable flops?"}
  Q -->|no| W(["warning: no flops can go on a chain"])
  Q -->|yes| S2["2. decide how many chains each domain gets"]
  S2 --> S3["3. check port names are free;<br/>create or reuse the scan enable port"]
  S3 --> S4["4. cut each domain into balanced chains;<br/>swap cells and wire SI, SE, Q"]
  S4 --> Z(["ScanResult"])
```

### Step 1: is this flop usable, and which domain is it in?

```mermaid
flowchart TD
  A(["each instance that elaborated to a flop"]) --> B{"cell is in the library?"}
  B -->|"no (Yosys generic flop)"| X1["exclude: map to a cell library first"]
  B -->|yes| C["note which outputs this instance actually uses (Q, QN ...)"]
  C --> D{"seen this cell + outputs before?"}
  D -->|yes| F["reuse the cached answer"]
  D -->|no| E["find_scan_equivalent(lib, cell, used outputs);<br/>then find_tap: the output that carries the state"]
  E --> F
  F --> G{"scan cell and tap found?"}
  G -->|no| X2["exclude: no scan equivalent,<br/>or scan cell has no state output"]
  G -->|yes| H{"clock pin connected?"}
  H -->|no| X3["exclude: clock not connected"]
  H -->|yes| I["trace the clock back through Buf and Not gates;<br/>each Not flips the edge"]
  I --> J{"reached an input port?"}
  J -->|no| X4["exclude: gated or generated clock"]
  J -->|yes| K["add to domain (clock port, edge)"]
```

Grouping by clock **and** edge means each chain is clocked by one clock on one edge,
so chains never need lockup latches.

### Step 2: how many chains per domain

```mermaid
flowchart TD
  A(["chains per domain, starting at 1 each"]) --> B{"--chains N given?"}
  B -->|yes| C{"N at least the<br/>number of domains?"}
  C -->|no| X["error: each domain needs its own chain"]
  C -->|yes| D["hand out the extra chains one at a time<br/>to the domain whose chains are longest"]
  D --> E{"ran out of flops to split?"}
  E -->|yes| W["warning: made fewer chains"]
  E -->|no| Z(["done"])
  B -->|no| F{"--max-length L given?"}
  F -->|yes| G["chains = ceil(flops / L) per domain"] --> Z
  F -->|no| Z
```

### Step 3: ports

- `test_si<k>` and `test_so<k>` must not exist yet (error naming the option to
  change: `--si-prefix`, `--so-prefix`).
- If `test_se` already exists as a one-bit input it is reused (with a warning);
  otherwise it is added.
- If a scan cell needs an **active-low** scan enable, one inverter cell from the
  library makes `test_se_n` on first use (`find_inverter` prefers cells named INV).

### Step 4: stitch

```mermaid
flowchart TD
  A(["for each domain, for each of its chains"]) --> B["length = flops / chains,<br/>plus 1 for the first (flops mod chains) chains"]
  B --> C["prev = new input port test_si(k)"]
  C --> D["for each flop in this chain (file order)"]
  D --> E{"already the scan cell?"}
  E -->|yes| F["restitch: remember its old scan enable<br/>(if it was an input port, hold it inactive in checks;<br/>if it was logic, warn)"]
  E -->|no| G["swap the cell type to the scan cell"]
  F & G --> H["connect SI = prev, SE = test_se (or test_se_n)"]
  H --> I{"tap output pin connected?"}
  I -->|no| J["connect it to a fresh net"]
  I -->|yes| K
  J --> K["record ScanCell (name, SI pin, out pin, inverting);<br/>prev = tap net"]
  K --> D
  D -->|chain done| L["new output port test_so(k) = assign from prev"]
  L --> A
```

If a cell has no Q and the chain leaves by QN, the cell is marked `inverting`.
`ScanChain::inverted()` tells whether the scan-out sees the scan-in data inverted
overall.

## Finding scan cells by function

### `find_scan_pins(cell)`: is this a mux-scan flop, and which pins are SI and SE?

```mermaid
flowchart TD
  A(["find_scan_pins(cell)"]) --> B{"has an ff group with<br/>a parsable next_state?"}
  B -->|no| N(["not a scan cell"])
  B -->|yes| C["SI candidates: pins with signal_type test_scan_in,<br/>else common names SI, SCD, TI, SDI, ..."]
  C --> D["SE candidates: pins with signal_type test_scan_enable,<br/>else common names SE, SCE, TE, SEN, ..."]
  D --> E["for each SI, SE pair, for SE active-high then active-low"]
  E --> F{"with SE active, for every other input and state:<br/>next state = SI?"}
  F -->|no| E
  F -->|yes| G{"with SE inactive, flipping SI<br/>never changes the next state?"}
  G -->|no| E
  G -->|yes| Y(["ScanPins: si, se, active level"])
  E -->|no pair works| N
```

The names only suggest candidates; the truth-table test decides. That is how
active-low scan enables and unusual pin names work.

### `find_scan_equivalent(lib, flop)`: which scan cell can replace this flop?

```mermaid
flowchart TD
  A(["find_scan_equivalent(lib, f, used outputs)"]) --> B{"f itself is a scan cell?"}
  B -->|yes| Y0(["f, with its own scan pins"])
  B -->|no| C["for each library cell s, in name order"]
  C --> D{"s is a flop with scan pins?"}
  D -->|no| C
  D -->|yes| E{"s's other inputs (without SI, SE)<br/>are exactly f's inputs?"}
  E -->|no| C
  E -->|yes| F{"both have clear / preset,<br/>or both lack them?"}
  F -->|no| C
  F -->|yes| G["mission mode: SE inactive, SI = 0"]
  G --> H{"for every input and state, same:<br/>next state, clock, clear, preset,<br/>and every used output pin's function?"}
  H -->|no| C
  H -->|yes| I["edit distance between names;<br/>keep the closest (DFFX1 picks SDFFX1)"]
  I --> C
  C -->|done| Z(["best match, or nothing"])
```

`find_tap(cell, used)` picks the output pin that carries the state, preferring one the
instance already uses, so stitching adds no new net: a used Q, then a used QN (marked
inverting), then an unused Q, then an unused QN. This is what Design Compiler does when a
flop only has QN connected, and it keeps flop names stable for `rith equiv`.

## Checking the result

### `verify_mission_mode(before, after, result)`

```mermaid
flowchart TD
  A(["verify_mission_mode"]) --> B["pair flops by instance using flop_of_instance;<br/>every old flop must still exist"]
  B --> C["observation points: every old output (must still exist)<br/>and every flop's next state"]
  C --> D["for each block of 64 random patterns"]
  D --> E["same random values on shared inputs and paired flops;<br/>old scan enables held inactive"]
  E --> F["after-design only: test_se = 0;<br/>scan-in ports and new flops random (they must not matter)"]
  F --> G["simulate both"]
  G --> H{"any observation point differs?"}
  H -->|yes| X(["fail: mission mode differs at ..."])
  H -->|no| D
  D -->|all blocks| OK(["ok"])
```

The TPI command reuses this check with the test point enable in place of `test_se`.

### `verify_shift(after, result)`

```mermaid
flowchart TD
  A(["verify_shift"]) --> B["for each chain: its scan-in input, scan-out output,<br/>flop order, and the inversion seen before each cell"]
  B --> C["random start state in every flop"]
  C --> D["for cycle t = 0 .. longest chain + 2"]
  D --> E["random values on all inputs, test_se = 1;<br/>remember what went into each scan-in"]
  E --> F["simulate"]
  F --> G{"each cell j (once t > j):<br/>holds scan-in from t - 1 - j cycles ago<br/>(inverted if needed)?"}
  G -->|no| X1(["fail: cell j does not hold the shifted data"])
  G -->|yes| H{"once t >= length:<br/>scan-out = scan-in from 'length' cycles ago?"}
  H -->|no| X2(["fail: scan-out does not match"])
  H -->|yes| I["clock: every flop takes its D value"]
  I --> D
  D -->|done| OK(["ok"])
```

Because the other inputs are random every cycle, this also proves the shift path
does not depend on them.

## Output files (`scan_out.cpp`)

| Function | File | Contents |
|---|---|---|
| `write_scan_report` | text report | scan enable, chain count and lengths, clock and edge per chain, cell swaps, flops left off and why, warnings, chain order |
| `write_def_scanchains` | DEF 5.8 | a `SCANCHAINS` section: per chain a `PARTITION` (clock and edge), start pin, `FLOATING` cell list with IN and OUT pins, stop pin. Layout tools (OpenROAD, Innovus) can reorder within it. |
| `write_stil_protocol` | STIL 1.0 | signals, signal groups, scan structures (length, inversion, cells), a 100 ns timing table, `capture`, `capture_<clk>` and `load_unload` procedures, `test_setup` macro |
| `write_tessent_dofile` | Tessent dofile | `add_clocks`, constrain `test_se` to 0, `add_scan_groups`, `add_scan_chains` |
| `write_tessent_testproc` | Tessent test procedures | timeplate, `test_setup`, `shift`, `load_unload` with `apply shift <longest>` |

```mermaid
flowchart LR
  R["ScanResult + Design"] --> RP["write_scan_report"] --> F1["scan.rpt"]
  R --> DEF["write_def_scanchains"] --> F2["scan.def"]
  R --> STIL["write_stil_protocol"] --> F3["scan.spf"]
  R --> TD["write_tessent_dofile"] --> F4["prefix.dofile"]
  R --> TP["write_tessent_testproc"] --> F5["prefix.testproc"]
```

The STIL and Tessent files have not yet been tried in the commercial tools.
