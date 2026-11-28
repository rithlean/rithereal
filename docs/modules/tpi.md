# Test point insertion (TPI)

Files: `include/rith/tpi.hpp`, `src/tpi.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Some faults are very hard to detect with random patterns: a net that is almost
always 1, or a net whose changes almost never reach an output. A **test point** is a
little extra logic that fixes that during test, and does nothing in mission mode.

| Kind | Added hardware | Effect during test (`tp_en` = 1) |
|---|---|---|
| Observe | a new flop that captures the net | faults behind the net become visible at that flop |
| Control-0 | `net AND NOT(tp_en AND ctl)` | consumers see the net forced to 0 when the point's flop `ctl` holds 1 |
| Control-1 | `net OR (tp_en AND ctl)` | consumers see the net forced to 1 when `ctl` holds 1 |

`ctl` is a new flop that just holds the value scanned into it. `tp_en` is a new input
that is 0 in mission mode. Primary outputs keep reading the *original* net, so with
`tp_en = 0` the design behaves exactly as before.

```mermaid
flowchart LR
  subgraph C1["Control-1 point on net n"]
    N1["n"] --> OR1["OR"]
    EN1["tp_en"] --> AND1["AND"]
    CTL1["ctl flop<br/>(D = Q)"] --> AND1
    AND1 --> OR1
    OR1 --> CON1["consumers of n"]
    N1 --> PO1["primary output<br/>(unchanged)"]
  end
  subgraph OB["Observe point on net m"]
    M["m"] --> OBF["new flop:<br/>captured in scan"]
    M --> CON2["consumers of m (unchanged)"]
  end
```

The module has three jobs:

1. **Choose** points on a `Netlist` (`select_test_points`), the classic baseline.
2. **Measure** coverage with and without them (`apply_test_points`, `random_coverage`).
3. **Insert** them into a mapped `Design` with real library cells (`insert_test_points`).

## COP with test points (`cop_full`)

The selector needs to know how a test point would change COP numbers without really
building it. `cop_full` is the [COP](testability.md) calculation with test points
applied virtually. Each control point is active with probability `kQ = 0.5`.

It keeps three values per net:

- `p1`: chance the original net is 1;
- `seen`: chance its consumers see 1 (after a control point, if any);
- `obs`: chance a change on the original net is observed.

```mermaid
flowchart TD
  A(["cop_full(nl, points)"]) --> B["mark each net: observe / control-0 / control-1"]
  B --> C["forward in topo_order: p1 from the fan-ins' 'seen' values<br/>(same rules as COP)"]
  C --> D["seen = p1;<br/>control-0: seen = p1 × (1 - kQ);<br/>control-1: seen = seen + (1 - seen) × kQ"]
  D --> E["backward start: outputs and observe points see the raw net;<br/>flop D inputs see the controlled net"]
  E --> F["reverse topo_order: a change on the raw net is seen if it<br/>reaches an output / observe point directly, or passes the control gate<br/>(blocked with chance kQ per control point) and is then seen"]
  F --> G["spread obs to fan-ins with the usual side-input rules,<br/>using 'seen' values"]
  G --> Z(["p1, seen, obs per net"])
```

## The cost the selector minimises

For each stem fault the chance one random pattern detects it is
`pd = p1 × obs` (SA0) or `(1 − p1) × obs` (SA1). After `N` patterns the chance it is
still undetected is `(1 − pd)^N`. The cost is the sum of that over all target nets:
the **expected number of faults that escape** `N` random patterns.

```mermaid
flowchart LR
  P["p1, obs of net g"] --> D0["pd0 = p1 × obs"] --> E0["escape0 = (1 - pd0)^N"]
  P --> D1["pd1 = (1 - p1) × obs"] --> E1["escape1 = (1 - pd1)^N"]
  E0 & E1 --> S["sum over target nets = cost"]
```

`escape()` computes `(1 − pd)^N` as `exp(N × log1p(−pd))` so tiny probabilities do not
round to zero.

## Choosing points (`select_test_points`)

A greedy loop: each round, find the single best point to add, add it, repeat.

```mermaid
flowchart TD
  A(["select_test_points(nl, cfg)"]) --> B["live = gates some output or flop can see;<br/>targets = live"]
  B --> C["eligible nets: live, not an input, constant or clock net,<br/>and allowed by the mask (Design mode: real nets only)"]
  C --> R0["refine(): fault-simulate with no points;<br/>targets = only nets with an undetected fault"]
  R0 --> D["cur = cop_full(points so far); cost[0]"]
  D --> L{"points < max_points?"}
  L -->|no| Z(["TpiResult: points, cost after each"])
  L -->|yes| S["cheap scores for every eligible net (below)"]
  S --> T["shortlist the best few of each kind:<br/>half observe, a quarter each control-0 / control-1<br/>(64 in total by default)"]
  T --> U{"shortlist empty?"}
  U -->|yes| Z
  U -->|no| V["exact re-score in parallel:<br/>cost of cop_full(points + candidate) per candidate"]
  V --> W{"best candidate removes at least<br/>min_gain expected escapes?"}
  W -->|no| Z
  W -->|yes| X["add it"]
  X --> Y{"points a multiple of refine_every (4)?"}
  Y -->|yes| R["refine(): fault-simulate with the points so far;<br/>retarget to nets whose faults still escape"]
  Y -->|no| D
  R --> D
```

**Cheap scores** (quick guesses used only to build the shortlist):

- *Observe at g*: the escapes g would lose if fully observed, plus half of each
  fan-in's escapes scaled by how poorly that fan-in is observed now.
- *Control-0 at g*: the escapes of g's gate consumers × chance g is 1 now (forcing 0
  helps most when the net is usually 1).
- *Control-1 at g*: the same × chance g is 0 now.

**Why refine?** COP treats signals as independent, which is wrong where a signal
splits and later rejoins. It can then aim points at nets whose faults are in fact
already detected. Every 4 points, a real fault simulation (`random_coverage`) shows
which nets really have escaping faults, and the cost is aimed only at those.
`--cop-only` (`refine_every = 0`) turns this off.

## Building test points as gates (`apply_test_points`)

Used to measure coverage, and by `rith tpi -o` on a BENCH netlist.

```mermaid
flowchart TD
  A(["apply_test_points(nl, points, enable)"]) --> B["name a control net rith_tp_c(k) for each control point<br/>(two control points on one net: error)"]
  B --> C["copy every gate; where a fan-in is a controlled net,<br/>read rith_tp_c(k) instead"]
  C --> D["add the enable as an input (or a Const1 'rith_tp_en' if no name)"]
  D --> E["for each point k"]
  E --> F{"kind"}
  F -->|observe| G["Dff rith_tp_obs(k) with D = net"]
  F -->|control| H["Dff rith_tp_ctl(k) with D = its own Q (holds its value);<br/>g = NAND(en, ctl) for control-0, AND(en, ctl) for control-1;<br/>rith_tp_c(k) = AND(net, g) or OR(net, g)"]
  G & H --> E
  E -->|done| I["outputs still point at the original nets; finalize"]
  I --> Z(["new Netlist"])
```

New flops use the clock of the first clocked flop in the netlist.

## Measuring coverage (`random_coverage`)

```mermaid
flowchart TD
  A(["random_coverage(original, points, N)"]) --> B["nl = original, or apply_test_points if there are points"]
  B --> C["fault list of the ORIGINAL netlist, uncollapsed"]
  C --> D["map each fault's gate to the same-named gate in nl<br/>(a branch fault stays on its consumer pin)"]
  D --> E["hold the enable input at 1"]
  E --> F["run_random_patterns for N patterns (no early stop)"]
  F --> G["for each undetected fault: note its original net once"]
  G --> Z(["faults, detected, unused, undetected_nets"])
```

Measuring over the original logic's faults means "before" and "after" count exactly
the same faults; faults inside the new test logic are not counted.

## Inserting into a mapped design (`insert_test_points`)

```mermaid
flowchart TD
  A(["insert_test_points(design, lib, named points, cfg)"]) --> B{"clock given?"}
  B -->|no| B1["elaborate; trace each flop's clock to an input port;<br/>use the port that clocks the most flops"]
  B -->|yes| C
  B1 --> C["add input port tp_en (error if the name is used)"]
  C --> D["find_plain_flop: a D flop with 2 inputs, no set/reset,<br/>next_state = one pin, prefer X1 / _1 drive"]
  D --> E["find_logic_cell for AND2, NAND2, OR2, INV by truth table"]
  E --> F["for each point"]
  F --> G{"net exists?"}
  G -->|no| W["warning: skipped"] --> F
  G -->|yes| H{"kind"}
  H -->|observe| I["flop: D = net, Q = fresh net"] --> F
  H -->|control| J["c = fresh net; move every input pin and internal assign<br/>that read the net over to c (output ports keep the net)"]
  J --> K["ctl flop with D = Q"]
  K --> L{"control-0?"}
  L -->|yes| M["g = NAND2(tp_en, q); c = AND(net, g)"]
  L -->|"control-1, OR2 available"| N["g = AND(tp_en, q); c = OR2(net, g)"]
  L -->|"control-1, no OR2"| O["g = NAND2(tp_en, q); nb = INV(net); c = NAND2(nb, g)"]
  M & N & O --> F
  F -->|done| Z(["counts of flops and gates added, clock used, warnings"])
```

`AND` here is an AND2 cell if the library has one, else NAND2 followed by INV. The
new flops are plain D flops; running `rith scan` afterwards turns them into scan
flops and puts them on chains.
