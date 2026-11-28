# Test generation (ATPG)

Files: `include/rith/atpg.hpp`, `src/atpg.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Random patterns detect most faults cheaply, but some faults need one very specific
input combination. **ATPG** (automatic test pattern generation) works out such a
pattern for one target fault at a time, or proves that none exists. A fault with no
test is **redundant**: the logic around it can never show the difference.

The module has two parts:

1. `Podem`: generates a test for one fault with the PODEM algorithm.
2. `run_atpg`: the whole flow for a netlist: random patterns, PODEM for what is left,
   compaction.

Everything works in the [full-scan view](../README.md#words-used-on-these-pages):
inputs and flops are what the tester sets, outputs and flop D inputs are what it reads.

## Five values: good and faulty machine together

PODEM simulates two copies of the circuit at once: the **good** machine and the
**faulty** one (with the fault injected). Each net holds a pair of three-valued
values (0, 1 or X for "not decided yet"):

| Pair (good, faulty) | Name | Meaning |
|---|---|---|
| (0, 0) / (1, 1) | 0 / 1 | same in both machines |
| (1, 0) | D | the fault effect: good 1, faulty 0 |
| (0, 1) | D' | the fault effect the other way round |
| anything with an X | X | not settled yet |

A fault is **detected** when some output or flop input holds D or D'.

## The PODEM loop (`Podem::Impl::run`)

PODEM (Goel, 1981) only ever makes decisions on **inputs and scan flops**. Every other
value follows by simulation. That keeps the search small: at most two choices per input.

```mermaid
flowchart TD
  A(["generate(fault)"]) --> S["setup: all sources X,<br/>fault cone, values implied"]
  S --> D{"D or D' at an output<br/>or flop input?"}
  D -->|yes| OK(["detected: return the test cube"])
  D -->|no| O{"objective()<br/>is a test still possible?"}
  O -->|"yes: (net, value)"| B["backtrace() to an<br/>unassigned input"]
  B --> AS["assign the input, push the decision,<br/>imply (event-driven 3-valued sim)"]
  AS --> D
  O -->|no| BT{"a decision on the stack<br/>not yet flipped?"}
  BT -->|no| RED(["redundant: every choice tried"])
  BT -->|"yes, but backtrack limit reached"| AB(["aborted"])
  BT -->|yes| FL["pop flipped decisions (back to X);<br/>flip the newest one"]
  FL --> D
```

A test is no longer possible (and PODEM backtracks) when:

- the fault site already holds the stuck value, so the fault can never be excited; or
- the fault is excited but the **D-frontier** is empty: no gate with D or D' on an
  input still has an open output; or
- no D-frontier gate has an **X-path**: a path of still-open nets to an output.

All three tests are exact under three-valued simulation, so when every choice has been
tried the fault really is redundant.

### Objective (`objective`)

```mermaid
flowchart TD
  A(["objective()"]) --> B{"good value at the fault site"}
  B -->|"= stuck value"| N(["no test possible"])
  B -->|X| E(["excite: site = NOT stuck"])
  B -->|"= NOT stuck"| F["D-frontier: gates with an effect<br/>on an input and an open output"]
  F --> G["keep those with an X-path;<br/>pick the lowest SCOAP CO"]
  G --> H{"any left?"}
  H -->|no| N
  H -->|yes| I(["propagate: set an X side input<br/>to the gate's non-controlling value"])
```

Non-controlling values: 1 for AND/NAND, 0 for OR/NOR. For XOR any value passes the
effect, so the easier one by SCOAP is asked for. For a MUX with the effect on a data
input, the select is set to pick that input.

### Backtrace (`backtrace`)

Backtrace walks from the objective back to an input, choosing one unassigned fan-in
at each gate and flipping the wanted value through inverting gates. The choice follows
Goel's rule with [SCOAP](testability.md) controllability:

| Situation | Example | Choose |
|---|---|---|
| every input must get the value | AND output 1 | the **hardest** input (largest CC) first, to fail fast |
| any one input will do | AND output 0 | the **easiest** input (smallest CC) |
| XOR / XNOR | | the easiest input; wanted value adjusted by the parity of the set inputs |
| MUX | | the select if a data input already has the value, else a data input |

## SOCRATES learning and unique sensitization

Plain PODEM only notices a dead end when simulation shows it. SOCRATES (Schulz,
Trischler and Sarfert, 1988) adds reasoning about values that **every** test must
have, called **necessary assignments**, so many dead ends, and many redundant
faults, are found without searching. `PodemConfig::learning` turns this on (the
default); `rith atpg --plain` turns it off for comparison.

```mermaid
flowchart TD
  L(["Podem(nl, learning = true)"]) --> SL["static learning, once:<br/>for each net n and value v, closure(n = v);<br/>keep contrapositives m = !w => n = !v"]
  F(["generate(fault)"]) --> SU["static unique sensitization:<br/>site = NOT stuck, plus non-controlling side inputs<br/>of every post-dominator of the fault"]
  SU --> C1{"closure conflicts?"}
  C1 -->|yes| RED(["redundant, no search needed"])
  C1 -->|no| LOOP["PODEM loop"]
  LOOP --> CHK{"a necessary assignment<br/>contradicted by simulation?"}
  CHK -->|yes| BT["backtrack"]
  CHK -->|no| DU["dynamic unique sensitization:<br/>common post-dominators of the live D-frontier;<br/>their side inputs; closure"]
  DU -->|conflict| BT
  DU -->|ok| OBJ["objective: an open necessary assignment, hardest first;<br/>else the usual frontier objective"]
```

### The implication engine (`Implicator::closure`)

Starting from a few required values (good machine), it adds what they force:

| Direction | Rule | Example |
|---|---|---|
| backward | an uncontrolled output needs every input non-controlling | AND = 1 needs all inputs 1 |
| backward | a controlled output with one input left open needs that one controlling | AND = 0, other inputs 1: the last is 0 |
| backward | XOR with one input open | value from the parity |
| forward | inputs that fix a gate fix its output | an AND input 0 makes the output 0 |
| learned | a stored implication fires | m = 0 => n = 1 |

If a net would need both 0 and 1, the required values are inconsistent: no test
exists below the current decisions. The closure stops after 2000 values; stopping
early only means fewer conclusions, never a wrong one.

### Post-dominators

A gate *d* post-dominates *x* when every path from *x* to an output or flop input
goes through *d*. They are computed once per netlist with the
Cooper-Harvey-Kennedy intersection on the gate graph plus a sink that every observed
net feeds. If the fault effect must pass *d*, then *d*'s side inputs that the fault
cannot reach must hold the non-controlling value.

### Checking it

Learning and pruning must never lose a test. The tests run the brute-force
comparison twice (plain and with learning), and check every learned implication
against exhaustive simulation of c17, s27 and 150 random circuits.

## The whole flow (`run_atpg`)

```mermaid
flowchart TD
  A(["run_atpg(netlist)"]) --> FL["collapsed fault list"]
  FL --> R["random phase: blocks of 64 patterns;<br/>keep only patterns that first detect a fault;<br/>stop after 8 useless blocks"]
  R --> P["for each undetected fault: PODEM"]
  P --> Q{"outcome"}
  Q -->|redundant| MR["mark redundant"]
  Q -->|aborted| MA["leave aborted"]
  Q -->|detected| X["fill X with random bits; add to the block"]
  X --> BL{"64 patterns waiting?"}
  BL -->|yes| SIM["fault-simulate the block (drops other faults too);<br/>each target must be detected, else an internal error"]
  BL -->|no| P
  SIM --> P
  P -->|done| C["reverse-order compaction: replay newest first,<br/>keep only patterns that detect something new"]
  C --> Z(["AtpgResult: patterns, counts, status per fault, errors"])
```

**Coverage** is detected faults over all faults. **Efficiency** is detected plus
proved-redundant faults over all faults: it says how much of the work ATPG finished.
Both are reported on uncollapsed faults.

## Choices the paper leaves open

These are documented in the header and kept stable so traces are reproducible:

- D-frontier gate: the one with an X-path and the lowest SCOAP CO, ties by gate id.
- XOR backtrace: the easiest unassigned input, value from the parity of set inputs.
- Each decision is flipped once at most (a complete search); `backtrack_limit` caps
  the number of flips, after which the fault is aborted, not called redundant.

## Checking it

`rith atpg x.bench --trace "n12 SA0"` prints every step (objective, backtrace,
backtrack) for one fault, for reading next to the paper.

The [tests](tests.md) compare PODEM with brute force: on every fault of c17, s27 and
150 random circuits with every gate type, PODEM with no backtrack limit must say
detected exactly when some input assignment detects the fault, and its cube must
detect it whatever the X bits are set to.

## Known limits

- Some redundancies need more reasoning than SOCRATES's direct and learned
  implications, mostly around XOR trees (c432, c499, c1355). A SAT fallback is
  the planned next step.
- PODEM runs on one thread; only fault simulation uses several.
- Patterns are written as plain text; STIL patterns are planned.
