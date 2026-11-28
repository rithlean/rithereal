# Faults and fault simulation

Files: `include/rith/fault.hpp`, `src/fault.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

This module answers: "if the chip had a defect here, would these test patterns
notice?" It has three parts:

1. `build_fault_list()` lists every single stuck-at fault and merges the ones that
   can never be told apart (collapsing).
2. `FaultSimulator` runs blocks of 64 patterns and marks which faults each block
   detects, using several threads.
3. `run_random_patterns()` feeds the simulator random patterns until coverage stops
   improving.

## Where faults sit

A `Fault` is `{gate, pin, stuck_at_one}`.

- `pin == kOutputPin` (-1): a **stem** fault on the gate's output net.
- `pin >= 0`: a **branch** fault on one input pin of the gate. Branch faults are
  only made where the driving net goes to two or more places (counting primary
  outputs). With a single consumer, the branch is the same wire as the stem.

```mermaid
flowchart LR
  A["gate a"] -->|"stem a: SA0, SA1"| S(("fan-out<br/>point"))
  S -->|"branch: b.in0 SA0, SA1"| B["gate b"]
  S -->|"branch: c.in1 SA0, SA1"| C["gate c"]
```

## `build_fault_list()`

```mermaid
flowchart TD
  A(["build_fault_list(nl, collapse)"]) --> B["count consumers of each net<br/>(fan-outs + primary outputs)"]
  B --> C["mark live gates: walk backwards from every output<br/>and every flop D input; stop at sources"]
  C --> D["stem faults: for each gate that is not a constant;<br/>dead gate: count 2 as unused;<br/>live gate: add SA0 and SA1"]
  D --> E["branch faults: for each input pin whose driver has 2+ consumers<br/>and is not a constant; dead consumer: 2 unused; else add SA0, SA1"]
  E --> F{"collapse?"}
  F -->|no| H
  F -->|yes| G["union-find: join equivalent faults per gate type"]
  G --> H["one representative per class<br/>(the earliest), weight = class size"]
  H --> Z(["FaultList: faults, weight, uncollapsed, unused"])
```

**Live** means some output or flop can see the gate. Faults in dead logic (and on
nets like clock and reset ports, which feed nothing in the scan view) can never be
detected, so they are counted separately as `unused` instead of lowering coverage.

### Which faults are equivalent

For each gate, an input fault and an output fault that always give the same wrong
output are joined. "Input fault" means the branch fault if there is one, else the
driver's stem fault.

| Gate | Joined |
|---|---|
| AND | any input SA0 = output SA0 |
| NAND | any input SA0 = output SA1 |
| OR | any input SA1 = output SA1 |
| NOR | any input SA1 = output SA0 |
| BUF | input SA0 = output SA0, input SA1 = output SA1 |
| NOT | input SA0 = output SA1, input SA1 = output SA0 |
| XOR, XNOR, MUX, DFF | nothing (no structural equivalence) |

The union-find always keeps the smaller index as the class leader, so the result is
the same on every run.

## `FaultSimulator`

The method is **parallel-pattern single-fault propagation (PPSFP)**: for one block of
64 patterns, simulate the good circuit once, then for each fault simulate only the
part of the circuit the fault changes.

```mermaid
sequenceDiagram
  participant Caller
  participant FS as FaultSimulator
  participant Sim as simulate()
  participant W as worker threads
  Caller->>FS: simulate_block(pi, state, valid)
  FS->>Sim: good values for 64 patterns
  Sim-->>FS: good_ (one word per gate)
  FS->>FS: list undetected faults (todo)
  FS->>W: hand out todo in chunks of 256
  loop each fault in a chunk
    W->>W: detect_mask(fault) = patterns that see it
  end
  W-->>FS: masks
  FS->>FS: mark detected faults, record first pattern, drop them
  FS-->>Caller: number newly detected
```

### `simulate_block()`

```mermaid
flowchart TD
  A(["simulate_block(pi, state, valid)"]) --> B["good_ = simulate(netlist, pi, state)"]
  B --> C["each worker's faulty copy = good_"]
  C --> D["todo = indices of undetected faults"]
  D --> E["start min(threads, todo/256 + 1) workers;<br/>each takes the next chunk of 256 from an atomic counter"]
  E --> F["mask[k] = detect_mask(fault) AND valid"]
  F --> G["join threads"]
  G --> H["for each fault with a non-zero mask:<br/>status = Detected;<br/>first pattern = patterns so far + lowest set bit;<br/>add to detected count and weight"]
  H --> I["patterns += number of valid bits"]
  I --> Z(["return newly detected"])
```

Detected faults are not simulated again (fault dropping), so later blocks get faster.

### `detect_mask()`: does this block see this fault?

```mermaid
flowchart TD
  A(["detect_mask(worker, fault)"]) --> B["stuck = all ones for SA1, all zeros for SA0"]
  B --> C{"stem fault?"}
  C -->|yes| D{"good value already<br/>equals stuck in every pattern?"}
  D -->|yes| N(["0: fault never activated"])
  D -->|no| P1["propagate(gate, stuck)"]
  C -->|"no: branch fault on pin p"| E{"gate is a Dff?"}
  E -->|yes| F(["good value on D XOR stuck:<br/>the D pin is itself observed"])
  E -->|no| G["re-evaluate the gate with pin p = stuck"]
  G --> H{"output changed?"}
  H -->|no| N
  H -->|yes| P2["propagate(gate, new output)"]
  P1 & P2 --> R(["mask of patterns where the<br/>difference reaches an output or flop"])
```

### `propagate()`: event-driven, level by level

```mermaid
flowchart TD
  A(["propagate(start, value)"]) --> B["faulty[start] = value; remember it"]
  B --> C{"start is observed?"}
  C -->|yes| C1["detected |= value XOR good"]
  C -->|no| D
  C1 --> D["queue start's fan-outs that are not sources,<br/>each in the bucket for its level"]
  D --> E["for each level above start, while gates are queued"]
  E --> F["take gate h from the bucket"]
  F --> G["v = eval_gate(h) on faulty values"]
  G --> H{"v equals good[h]?"}
  H -->|"yes: the fault effect died here"| F
  H -->|no| I["faulty[h] = v; remember h"]
  I --> J{"h observed?"}
  J -->|yes| J1["detected |= v XOR good[h]"]
  J -->|no| K
  J1 --> K["queue h's fan-outs"] --> F
  E -->|"no more queued"| L["reset every remembered gate to its good value"]
  L --> Z(["detected"])
```

"Observed" means a primary output or a flop's D input. Flops are not evaluated as
gates; their D pin is an observation point. Each thread has its own `Worker`
scratch space (faulty values, queue flags, level buckets), so threads never share
writable memory.

## `run_random_patterns()`

```mermaid
flowchart TD
  A(["run_random_patterns(sim, max, seed, patience, fixed)"]) --> B["random generator from seed; quiet = 0"]
  B --> C{"patterns < max and faults left<br/>and quiet < patience?"}
  C -->|no| Z(["result: patterns, detected, coverage"])
  C -->|yes| D["random words for every input and flop"]
  D --> E["hold 'fixed' inputs at their value<br/>(for example tp_en = 1)"]
  E --> F["valid = all 64 bits, or fewer for the last block"]
  F --> G["n = simulate_block"]
  G --> H{"n > 0?"}
  H -->|yes| H1["quiet = 0"] --> C
  H -->|no| H2["quiet + 1"] --> C
```

`patience = 0` means "never stop early". Coverage is reported two ways: over the
collapsed list (`coverage`) and weighted by class size (`coverage_uncollapsed`).

`describe()` prints a fault for people, for example `n12 SA0` for a stem or
`n12 -> U5.in1 SA1` for a branch.
