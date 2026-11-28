# Structural design view

Files: `include/rith/design.hpp`, `src/design.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

A `Design` keeps a flat Verilog netlist **as written**: library cell instances with
named pin connections, gate primitives, `assign` statements, ports and buses. Passes
that edit the circuit (scan insertion, test point insertion) work on it and write it
back with the original cell names, so the result can go straight back into Design
Compiler, Genus, Tessent or OpenROAD.

`design.cpp` has four parts:

```mermaid
flowchart LR
  T["Verilog text"] --> LEX["Lexer<br/>text to tokens"]
  LEX --> PAR["Parser<br/>tokens to Design"]
  PAR --> D["Design"]
  D --> ELAB["Elaborator<br/>Design + Library to Netlist"]
  D --> WR["write_verilog_design<br/>Design to Verilog text"]
  ELAB --> N["Elaboration:<br/>Netlist + flop_of_instance"]
```

## The data model

| Type | Holds |
|---|---|
| `NetId` | Number of a one-bit net. Special values: `kNoNet` (pin left open), `kConst0Net` (`1'b0`), `kConst1Net` (`1'b1`). A bus bit is its own net, named like `data[3]`. |
| `PortDecl` | Port name, direction, optional bit range. |
| `Instance` | Cell or primitive type, instance name, `positional` (primitives: first connection is the output), list of `PinConn {pin, net}`, source line. |
| `Assign` | `lhs = rhs` or `lhs = ~rhs`; `rhs` may be a constant. |
| `Design` | Name, ports, buses, instances, assigns, the order statements were written in, and the net name table. |

Helpers used by the editing passes: `net()` (find or create), `find_net()`,
`fresh_net(prefix)` and `fresh_instance_name(prefix)` (a name not used yet),
`add_port()`, `add_instance()`, `add_assign()`, `Instance::connect()`.

## Lexer

```mermaid
flowchart TD
  A(["Lexer::run"]) --> S["skip(): spaces, newlines (counted),<br/>// and /* */ comments,<br/>(* attributes *) from Yosys,<br/>` compiler directives"]
  S --> B{"next character"}
  B -->|end| Z(["tokens + End"])
  B -->|backslash| E["escaped identifier: up to the next space"] --> S
  B -->|"letter, _ or $"| I["identifier"] --> S
  B -->|"digit or '"| N["number, including sized forms like 1'b0, 4'hF"] --> S
  B -->|other| Y["one-character symbol"] --> S
```

## Parser

The parser makes one pass over the tokens. It has no knowledge of cells; the
elaborator checks those later.

```mermaid
flowchart TD
  A(["Parser::run"]) --> B{"more tokens?"}
  B -->|yes| C["expect 'module', read its name"]
  C --> D{"name is rith_dff,<br/>rith_dffn or rith_mux2?"}
  D -->|yes| D1["skip to endmodule<br/>(rithereal's own helper modules)"] --> B
  D -->|no| E{"already have a<br/>top module?"}
  E -->|yes| X1["ParseError: hierarchy not supported, flatten first"]
  E -->|no| F["module_body()"] --> B
  B -->|no| G{"found a module?"}
  G -->|no| X2["ParseError: no module found"]
  G -->|yes| H["order ports as in the header;<br/>non-ANSI ports take their direction from the body;<br/>keep body-only ports at the end"]
  H --> Z(["Design"])
```

```mermaid
flowchart TD
  A(["module_body"]) --> P{"'#' parameters?"}
  P -->|yes| X0["ParseError: parameterised modules not supported"]
  P -->|no| H["port list in '( )':<br/>ANSI 'input [3:0] a' declares now,<br/>non-ANSI 'a' only records the name"]
  H --> L["read next module item keyword"]
  L --> K{"keyword"}
  K -->|endmodule| Z(["done"])
  K -->|"input, output, wire, reg, logic, tri"| DEC["declaration: optional range, names;<br/>buses remembered; port bits become nets"] --> L
  K -->|"supply0 / supply1 / inout"| X1["ParseError: not supported"]
  K -->|assign| AS["assign_stmt: lhs = net, ~net or constant;<br/>comma lists allowed"] --> L
  K -->|"and, nand, or, nor, xor, xnor, not, buf"| PR["primitive: optional name,<br/>positional (out, in, in, ...)"] --> L
  K -->|"always, initial, function, task,<br/>parameter, generate ..."| X2["ParseError: behavioural code not supported"]
  K -->|any other word| CE["cell instance: type name ( .PIN(net), ... );<br/>named connections only; empty () = open pin"] --> L
```

A net reference (`net_ref`) is a constant (`1'b0`, `0`, `1'h1`), a bus bit
(`data[3]`), a one-bit bus used by its name, or a plain net. Concatenations
(`{a, b}`), part-selects (`a[3:0]`), multi-bit constants and `x`/`z` values give a
clear error.

## Elaborator: Design to gates

`elaborate()` builds a `Netlist` of primitive gates from the `Design`. It also fills
`flop_of_instance`, which says for each instance which `Dff` gate it became, so the
scan and TPI passes can map results back to instances.

```mermaid
flowchart TD
  A(["Elaborator::run"]) --> B["every input port bit becomes an Input gate"]
  B --> C["walk statements in the order written"]
  C --> D{"statement"}
  D -->|assign| AS["assign(): constant gives Const0/Const1;<br/>net gives Buf; ~net gives Not"] --> C
  D -->|instance| IN["instance()"] --> C
  C -->|done| E["each output port bit: add_output,<br/>or ParseError 'never driven'"]
  E --> F{"flops with async set/reset seen?"}
  F -->|yes| W["add a warning: assumed inactive in test mode"] --> G
  F -->|no| G["Netlist::finalize()"]
  G --> Z(["Elaboration"])
```

Constant nets become two shared gates, `rith_const0` and `rith_const1`, created on
first use.

### `instance()`: what kind of cell is it?

```mermaid
flowchart TD
  A(["instance(inst)"]) --> P{"positional<br/>(gate primitive)?"}
  P -->|yes| P1["one gate of that type:<br/>output = conns[0], inputs = the rest"] --> Z(["done"])
  P -->|no| Y{"cell type"}
  Y -->|"$_BUF_, $_NOT_, $_AND_ ... $_XNOR_"| Y1["one gate (Yosys generic cell, pins A, B, Y)"] --> Z
  Y -->|"$_ANDNOT_ / $_ORNOT_"| Y2["NOT on B, then AND / OR"] --> Z
  Y -->|"$_MUX_ or rith_mux2"| Y3["Mux2 (S, A, B)"] --> Z
  Y -->|"$_DFF_P_, $_DFF_N_, rith_dff, rith_dffn"| Y4["Dff on Q from D, clock C, edge from the name;<br/>record flop_of_instance"] --> Z
  Y -->|"other $_DFF*, $_SDFF*, $_DLATCH*"| X1["ParseError: Yosys cell not supported yet"]
  Y -->|"in the Liberty library"| L["library_cell()"] --> Z
  Y -->|unknown| X2["ParseError: unknown cell (pass its Liberty library)"]
```

### `library_cell()`: expand a Liberty cell into gates

```mermaid
flowchart TD
  A(["library_cell(cell, inst)"]) --> B{"latch or statetable?"}
  B -->|yes| X1["ParseError: not supported yet (latches, clock gates)"]
  B -->|no| C{"every connected pin<br/>exists on the cell?"}
  C -->|no| X2["ParseError: cell has no pin ..."]
  C -->|yes| D{"cell has an ff group?"}
  D -->|yes| E["name the flop after the net on the output<br/>whose function is just IQ (the Q pin),<br/>else 'inst/IQ'"]
  E --> F["build(next_state) gives the D logic;<br/>define the flop as Dff(D)"]
  F --> G{"clocked_on is a single pin<br/>or NOT of a pin?"}
  G -->|no| X3["ParseError: only a single clock pin supported"]
  G -->|yes| H["set_clock(pin's net, falling edge if negated);<br/>count async clear/preset;<br/>record flop_of_instance"]
  D -->|no| I
  H --> I["for each connected output pin<br/>(except the Q pin already done):<br/>build(its function) onto the pin's net"]
  I --> Z(["done"])
```

`build(expr, out)` turns a `LibExpr` tree into gates, recursively:

```mermaid
flowchart TD
  A(["build(expr, out)"]) --> T{"expr"}
  T -->|"0 / 1"| C["Const0 / Const1 gate"]
  T -->|"Var = IQ"| Q["the flop gate"]
  T -->|"Var = IQN"| QN["one shared Not of the flop"]
  T -->|"Var = input pin"| V["the net connected to that pin<br/>(error if the pin is open)"]
  T -->|"Not of And / Or / Xor"| NN["one Nand / Nor / Xnor gate"]
  T -->|"Not of anything else"| N["Not gate"]
  T -->|"And / Or / Xor"| G["one gate with a fan-in per argument"]
  NN & N & G --> R["inputs come from build(arg) each;<br/>the gate is named 'out' if given,<br/>else a fresh 'inst/$k' net"]
  V & Q & QN --> S{"out given?"}
  S -->|yes| B["add a Buf onto out"]
  S -->|no| U["use the net directly"]
```

## Writer (`write_verilog_design`)

```mermaid
flowchart TD
  A(["write_verilog_design(out, d)"]) --> B["module header with port names"]
  B --> C["input / output declarations, with ranges"]
  C --> D["one wire per net that is not a port;<br/>bus bits declared once as 'wire [m:l] bus'"]
  D --> E["statements in the original order"]
  E --> F{"statement"}
  F -->|assign| G["assign lhs = [~]rhs"]
  F -->|primitive| H["type name ( out, in, ... )"]
  F -->|cell| I["type name ( .PIN(net), ... ), open pins as .PIN()"]
  G & H & I --> E
  E -->|done| J["endmodule; then rith_dff / rith_dffn / rith_mux2 bodies if used"]
  J --> Z(["done"])
```

Bus bits keep their bus syntax (`data[3]`), other names are escaped only when they
need it, and constants print as `1'b0` / `1'b1`. The design round-trips: reading
and writing again gives the same netlist.
