# Testability: SCOAP and COP

Files: `include/rith/testability.hpp`, `src/testability.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Before simulating any faults, you can estimate how hard each net is to test just by
looking at the circuit's structure. Both measures here do that in two sweeps:

- a **forward** sweep (inputs to outputs) for *controllability*: how hard is it to
  make this net 0 or 1?
- a **backward** sweep (outputs to inputs) for *observability*: how hard is it to see
  this net at an output?

Both use the full-scan view: flops count as inputs (their Q) and as outputs (their D).

| Measure | Per net | Meaning | Good value |
|---|---|---|---|
| SCOAP `cc0`, `cc1` | integer | effort to set the net to 0 / 1 | small |
| SCOAP `co` | integer | effort to see the net | small (0 at an output) |
| COP `p1` | probability | chance the net is 1 under random inputs | near 0.5 |
| COP `obs` | probability | chance a change on the net reaches an output | near 1 |

COP also gives the chance a random pattern detects a stem fault:
`detect_sa0 = p1 × obs` and `detect_sa1 = (1 − p1) × obs`. `rith testability` sorts
nets by the smaller of the two to list the hardest ones. These numbers are also the
features and the baseline for [test point insertion](tpi.md).

## Overall flow

```mermaid
flowchart LR
  N["finalized Netlist"] --> F["forward sweep in topo_order:<br/>controllability"]
  F --> B["backward sweep in reverse topo_order:<br/>observability"]
  B --> R["Scoap or Cop<br/>(one entry per net)"]
```

## SCOAP (`compute_scoap`)

Numbers are capped at `kScoapInf` (2^30), which means "impossible".

### Forward: controllability

```mermaid
flowchart TD
  A(["for each gate g in topo_order"]) --> T{"type"}
  T -->|"Input or Dff"| S["cc0 = cc1 = 1"]
  T -->|Const0| K0["cc0 = 0 (cc1 stays impossible)"]
  T -->|Const1| K1["cc1 = 0 (cc0 stays impossible)"]
  T -->|"Buf / Not"| BN["copy from the input (swap 0 and 1 for Not), + 1"]
  T -->|"And / Nand / Or / Nor"| AO["controlling value: the cheapest single input, + 1<br/>non-controlling: sum of all inputs, + 1<br/>(swap for Nand / Nor)"]
  T -->|"Xor / Xnor"| XO["track cheapest even and odd parity input by input;<br/>cc0 = even + 1, cc1 = odd + 1 (swap for Xnor)"]
  T -->|Mux2| MX["cc0 = min(sel 0 + in0 at 0, sel 1 + in1 at 0) + 1;<br/>cc1 the same with 1"]
```

Example for AND: to get a 0 you only need one input at 0 (take the cheapest); to get
a 1 you need all inputs at 1 (add them up).

### Backward: observability

```mermaid
flowchart TD
  A(["start: co = 0 at every primary output and flop D input,<br/>impossible elsewhere"]) --> B["for each gate g in reverse topo_order"]
  B --> C{"g is a source or<br/>co[g] impossible?"}
  C -->|yes| B
  C -->|no| D["for each input pin p of g"]
  D --> E{"gate type: cost of the<br/>other inputs (side)"}
  E -->|"Buf / Not"| E0["0"]
  E -->|"And / Nand"| E1["sum of cc1 of the others"]
  E -->|"Or / Nor"| E2["sum of cc0 of the others"]
  E -->|"Xor / Xnor"| E3["sum of the cheaper of cc0, cc1 of the others"]
  E -->|"Mux2 select"| E4["cheapest way to make the data inputs differ"]
  E -->|"Mux2 data in0 / in1"| E5["cc0 / cc1 of the select"]
  E0 & E1 & E2 & E3 & E4 & E5 --> F["offer co[g] + side + 1 to the input net;<br/>the net keeps the smallest offer over all its branches"]
  F --> D
```

## COP (`compute_cop`)

### Forward: probability of 1

```mermaid
flowchart TD
  A(["for each gate g in topo_order"]) --> T{"type"}
  T -->|"Input or Dff"| S["p1 = 0.5"]
  T -->|"Const0 / Const1"| K["p1 = 0 / 1"]
  T -->|"Buf / Not"| BN["p1 = p, or 1 - p"]
  T -->|"And / Nand"| AN["product of input p1 (1 - that for Nand)"]
  T -->|"Or / Nor"| OR["1 - product of (1 - p1) (no 1 - for Nor)"]
  T -->|"Xor / Xnor"| XR["fold: q = q(1 - r) + (1 - q) r (1 - that for Xnor)"]
  T -->|Mux2| MX["(1 - s) p(in0) + s p(in1)"]
```

Inputs are treated as independent. That is exact for trees but only an estimate
where a signal splits and later rejoins (reconvergent fan-out). This is why TPI
re-checks its choices with real fault simulation.

### Backward: observability

```mermaid
flowchart TD
  A(["start: not_obs = 0 at outputs and flop D inputs, 1 elsewhere"]) --> B["for each gate g in reverse topo_order"]
  B --> C["obs[g] = 1 - not_obs[g]"]
  C --> D{"g is a source or obs[g] = 0?"}
  D -->|yes| B
  D -->|no| E["for each input pin p:<br/>side = chance the other inputs let a change through<br/>(And: product of their p1; Or: product of 1 - p1;<br/>Xor, Buf, Not: 1; Mux2: select or data rule)"]
  E --> F["not_obs[input] *= 1 - obs[g] × side"]
  F --> B
```

A net that feeds several gates is seen if *any* branch sees it. Treating the
branches as independent gives `obs(stem) = 1 − Π(1 − obs(branch))`, which is the
running product `not_obs`.
