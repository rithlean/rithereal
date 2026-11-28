# Netlist core

Files: `include/rith/netlist.hpp`, `src/netlist.cpp`, `include/rith/eval.hpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

`Netlist` is the data structure every analysis in rithereal works on. It is a flat
graph of primitive gates. Each gate drives exactly one net, and the net carries the
gate's name, so "gate" and "net" mean the same thing here and share one number, the
`GateId`.

Gate types (`GateType`): `Input`, `Const0`, `Const1`, `Buf`, `Not`, `And`, `Nand`,
`Or`, `Nor`, `Xor`, `Xnor`, `Mux2` (fan-ins are select, in0, in1) and `Dff` (one
fan-in, D; the clock is stored separately in `Gate::clock`). `Undefined` marks a net
that has been mentioned but not yet driven.

**Sources** (`is_source`) are inputs, constants and flops. In the full-scan view
they are where values start, so levelization begins from them.

## Life of a Netlist

A `Netlist` is built in two phases: first you add gates, then `finalize()` freezes
it and computes everything the analyses need.

```mermaid
stateDiagram-v2
  [*] --> Building
  Building --> Building: get_or_declare / define / add_gate / set_clock / add_output
  Building --> Finalized: finalize() succeeds
  Building --> Unusable: finalize() finds an undriven net or a loop
  Finalized --> Finalized: queries (fanins, fanouts, topo_order, level)
  Unusable --> [*]
  Finalized --> [*]
```

Readers can mention a net before the gate that drives it appears in the file
(BENCH and Verilog both allow this). `get_or_declare()` handles that by creating an
`Undefined` placeholder that `define()` fills in later.

## `define()`: giving a gate its type

```mermaid
flowchart TD
  A(["define(id, type, fanins)"]) --> B{"netlist already<br/>finalized?"}
  B -->|yes| E1["throw: netlist is finalized"]
  B -->|no| C{"gate already has<br/>a type?"}
  C -->|yes| E2["throw: more than one driver"]
  C -->|no| D{"fan-in count allowed<br/>for this type? (arity)"}
  D -->|no| E3["throw: wrong number of inputs"]
  D -->|yes| F{"every fan-in id<br/>exists?"}
  F -->|no| E4["throw: invalid fanin id"]
  F -->|yes| G["store type and fan-ins,<br/>append to definition order"]
  G --> H{"type is Input?"}
  H -->|yes| I["add to inputs list"]
  H -->|no| J{"type is Dff?"}
  I --> K(["done"])
  J -->|yes| L["add to flops list"] --> K
  J -->|no| K
```

Allowed fan-in counts (`arity()`): inputs and constants 0; `Buf`, `Not`, `Dff` 1;
`Mux2` 3; AND/OR/XOR families 1 or more.

## `finalize()`: freezing the graph

```mermaid
flowchart TD
  A(["finalize()"]) --> B{"already finalized?"}
  B -->|yes| Z(["return"])
  B -->|no| C["1. look for Undefined gates"]
  C --> D{"any undriven nets?"}
  D -->|yes| E1["throw: N undriven nets, listing up to 5"]
  D -->|no| F["2. pack fan-ins into one array<br/>with an offset per gate (CSR)"]
  F --> G["3. count each gate's consumers,<br/>then fill the fan-out array (CSR)"]
  G --> H["free the temporary per-gate fan-in vectors,<br/>mark finalized"]
  H --> I["4. levelize: put every source in the queue;<br/>for others, pending = number of fan-ins"]
  I --> J{"queue has a<br/>gate u?"}
  J -->|yes| K["for each consumer v of u that is not a source:<br/>level(v) = max(level(v), level(u) + 1);<br/>pending(v) -= 1; if 0, queue v"]
  K --> J
  J -->|no| L{"every gate<br/>was queued?"}
  L -->|no| E2["throw: combinational loop through ...<br/>(gates still pending)"]
  L -->|yes| M["depth = highest level"]
  M --> Z
```

Step 4 is Kahn's topological sort. Flops count as sources, so the wire from a
gate into a flop's D pin is "cut": loops through flops are fine, loops made only of
gates are an error.

**CSR** (compressed sparse row) means all fan-in lists sit back to back in one big
array, and `fanin_off_[g]` .. `fanin_off_[g+1]` marks gate `g`'s slice. It uses less
memory and is faster to walk than one small vector per gate.

```mermaid
flowchart LR
  subgraph offsets["fanin_off_"]
    o0["g0: 0"] --- o1["g1: 0"] --- o2["g2: 2"] --- o3["g3: 4"]
  end
  subgraph data["fanin_data_"]
    d0["0"] --- d1["1"] --- d2["0"] --- d3["2"]
  end
  o1 -. "g1 fan-ins = data[0..2)" .-> d0
  o2 -. "g2 fan-ins = data[2..4)" .-> d2
```

## Other functions in `netlist.cpp`

| Function | What it does |
|---|---|
| `to_string(GateType)` | Name of a type, e.g. `NAND`. |
| `gate_type_from_string()` | Reverse lookup, case-insensitive; accepts `BUFF`, `INV`, `MUX2`. |
| `find(net)` | Gate id by name, or `kNoGate`. |
| `add_gate()` | `get_or_declare` + `define` for a brand new name. |
| `set_clock()` | Records a flop's clock net and edge. |
| `add_output()` | Adds a primary output port pointing at a driver gate; rejects duplicates. |
| `fanins()` / `fanouts()` | Slices of the CSR arrays (fan-ins also work before finalize). |
| `count(type)` | How many gates of a type. |

## `eval.hpp`: one gate, 64 patterns

`eval_gate(type, n, in)` computes a gate's output for 64 patterns at once. A `Word`
is a 64-bit integer and each bit is one pattern, so an AND of 64 patterns is one `&`
instruction. `in(i)` is a small function that returns the word on input pin `i`;
the logic simulator passes real values, the fault simulator passes faulty ones.

```mermaid
flowchart TD
  A(["eval_gate(type, n, in)"]) --> T{"type"}
  T -->|Const0| R0["all zeros"]
  T -->|Const1| R1["all ones"]
  T -->|Buf| RB["in(0)"]
  T -->|Not| RN["NOT in(0)"]
  T -->|"And / Nand"| RA["AND of all inputs;<br/>invert for Nand"]
  T -->|"Or / Nor"| RO["OR of all inputs;<br/>invert for Nor"]
  T -->|"Xor / Xnor"| RX["XOR of all inputs;<br/>invert for Xnor"]
  T -->|Mux2| RM["s = in(0); pick in(1) where s is 0,<br/>in(2) where s is 1"]
  T -->|"Input, Dff, other"| RZ["0 (sources are set by the caller)"]
```
