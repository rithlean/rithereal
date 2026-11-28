# Logic simulation

Files: `include/rith/sim.hpp`, `src/sim.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Two jobs:

1. `simulate()` works out the value on every net for a block of 64 patterns at
   once (one bit per pattern), in the full-scan view.
2. `compare_by_simulation()` checks whether two netlists behave the same, by
   feeding both the same random patterns and comparing outputs and flop next states.
   `rith equiv` uses it.

## `simulate()`

```mermaid
flowchart TD
  A(["simulate(nl, pi_words, state_words)"]) --> B{"netlist finalized and<br/>word counts match?"}
  B -->|no| E["throw NetlistError"]
  B -->|yes| C["v = one Word per gate, all 0"]
  C --> D["put pi_words on the inputs<br/>and state_words on the flop outputs"]
  D --> F["walk gates in topo_order"]
  F --> G{"gate is Input<br/>or Dff?"}
  G -->|yes| F
  G -->|no| H["v[g] = eval_gate(type, fan-in words)"]
  H --> F
  F -->|all gates done| I(["return v"])
```

Because gates are visited in level order, every fan-in value is ready before the
gate that reads it. The value of a flop's D input (`v[fanins(flop)[0]]`) is the
flop's **next state**.

## `compare_by_simulation()`

```mermaid
flowchart TD
  A(["compare_by_simulation(a, b, blocks, seed)"]) --> B["match b's inputs to a's by name"]
  B --> C{"an input with fan-out<br/>exists in only one netlist?"}
  C -->|yes| E1["throw (clock-like inputs with no fan-out are allowed)"]
  C -->|no| D{"same flop count and every<br/>b flop name found in a?"}
  D -->|no| E2["throw: flop counts or names differ"]
  D -->|yes| F{"same outputs by name?"}
  F -->|no| E3["throw: output only in one netlist"]
  F -->|yes| G["observation list: every output,<br/>plus every flop's D input (next state)"]
  G --> H["repeat for each block"]
  H --> I["random words for a's inputs and flops;<br/>copy them to b's matching inputs and flops<br/>(b-only inputs get their own random words)"]
  I --> J["va = simulate(a), vb = simulate(b)"]
  J --> K{"any observation point<br/>differs?"}
  K -->|yes| L(["not equivalent: report the first mismatch"])
  K -->|no| M{"more blocks?"}
  M -->|yes| H
  M -->|no| N(["equivalent after blocks × 64 patterns"])
```

`EquivalenceOptions` adds two things for comparing scanned netlists. `tie` holds an
input at 0 or 1 in whichever netlist has it (for example `SE` off for mission mode);
`ignore` names ports that may exist in only one netlist: such an input gets random
values, and an output named there is not compared. A tied or ignored input may exist
in only one netlist even if it has fan-out.

This finds most real differences quickly, but it is a random check, not a formal
proof. Flops are matched by name, so both netlists must use the same flop names.
