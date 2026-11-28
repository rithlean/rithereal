# Netlist readers and writers

Files: `include/rith/io.hpp`, `src/bench.cpp`, `src/verilog.cpp`, `src/file_io.cpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

Getting circuits into and out of a `Netlist`:

| Function | File | Direction |
|---|---|---|
| `read_bench` / `write_bench` | `bench.cpp` | ISCAS BENCH format |
| `read_verilog` | `verilog.cpp` | Verilog to `Netlist` (via the [Design view](design.md)) |
| `write_verilog` | `verilog.cpp` | `Netlist` to generic gate-level Verilog |
| `verilog_identifier` | `verilog.cpp` | Escapes names that are not plain Verilog identifiers |
| `read_netlist_file` / `write_netlist_file` | `file_io.cpp` | Picks the format by file extension |

Errors in input files are thrown as `ParseError`, whose message is
`file:line: what went wrong`.

## Picking a format (`file_io.cpp`)

```mermaid
flowchart TD
  A(["read_netlist_file(path, lib)"]) --> B["open the file"]
  B --> C{"extension"}
  C -->|".v .sv .vg"| V["read_verilog(in, path, lib)"]
  C -->|".bench"| R["read_bench(in, path)"]
  C -->|other| E["throw: cannot tell the format"]
  R --> N["name the netlist after the file<br/>(no folder, no extension)"]
  V --> Z(["Netlist"])
  N --> Z
```

`write_netlist_file` does the same choice and calls `write_bench` or
`write_verilog`.

## BENCH reader (`read_bench`)

A BENCH file looks like:

```text
INPUT(G1)
OUTPUT(G22)
G10 = NAND(G1, G3)
G5  = DFF(G10)
```

```mermaid
flowchart TD
  A(["read_bench"]) --> B["read next line"]
  B -->|end of file| P["for each remembered OUTPUT name:<br/>find its gate or throw 'never driven';<br/>add_output"]
  P --> F["finalize()"] --> Z(["Netlist"])
  B --> C["drop '#' comment, trim"]
  C --> D{"empty?"}
  D -->|yes| B
  D -->|no| E{"has '(' and ')'?"}
  E -->|no| X1["ParseError: expected NAME(...)"]
  E -->|yes| G{"'=' before '('?"}
  G -->|"no: INPUT or OUTPUT line"| H{"keyword"}
  H -->|INPUT| I["define the net as an Input"] --> B
  H -->|OUTPUT| J["remember name and line for later"] --> B
  H -->|other| X2["ParseError"]
  G -->|"yes: gate line"| K["lhs = name before '=';<br/>type = word before '('"]
  K --> L{"known gate type?"}
  L -->|no| X3["ParseError: unknown gate type"]
  L -->|yes| M["split arguments on commas;<br/>get_or_declare each one"]
  M --> N["define(lhs, type, fan-ins)<br/>(a NetlistError becomes a ParseError with the line)"]
  N --> B
```

Outputs are resolved at the end because a BENCH file may list `OUTPUT(x)` before
the line that drives `x`.

`write_bench` writes a short header, then `INPUT(...)` and `OUTPUT(...)` lines, then
one `name = TYPE(fanins)` line per gate in definition order. It refuses outputs
whose port name differs from the driving net, since BENCH cannot express that.

## Verilog reader (`read_verilog`)

`read_verilog` is one line: it parses the file into a `Design` and elaborates it
into a `Netlist`. All the work is in [design.md](design.md).

```mermaid
flowchart LR
  A["Verilog text"] --> B["read_verilog_design"] --> C["Design"] --> D["elaborate(design, lib)"] --> E["Netlist"]
```

## Generic Verilog writer (`write_verilog`)

This writes a `Netlist` (not a `Design`) as Verilog using only gate primitives and
three small helper modules that rithereal defines itself: `rith_dff`, `rith_dffn`
(falling edge) and `rith_mux2`. It is used by `rith convert`.

```mermaid
flowchart TD
  A(["write_verilog(out, nl)"]) --> B["check no output has the same name as an input"]
  B --> C["note which helpers are needed:<br/>rising or falling flops, muxes;<br/>does any flop lack a clock?"]
  C --> D{"a flop has no clock?"}
  D -->|yes| D1["add a 'rith_clk' input<br/>(error if that name is taken)"]
  D -->|no| E
  D1 --> E["write module header and port list"]
  E --> F["declare inputs, outputs and a wire per internal gate"]
  F --> G["for each gate in definition order"]
  G --> H{"type"}
  H -->|Const0 / Const1| H1["assign net = 1'b0 / 1'b1"]
  H -->|Mux2| H2["rith_mux2 instance"]
  H -->|Dff| H3["rith_dff or rith_dffn instance"]
  H -->|other| H4["primitive: and / or / not ... (out, ins)"]
  H1 & H2 & H3 & H4 --> G
  G -->|done| I["assign renamed outputs"]
  I --> J["endmodule, then the helper module bodies used"]
  J --> Z(["done"])
```

## `verilog_identifier`

```mermaid
flowchart TD
  A(["verilog_identifier(name)"]) --> B{"starts with a letter or '_'<br/>and only letters, digits, '_', '$'?"}
  B -->|no| E["escaped: backslash + name + space"]
  B -->|yes| C{"a Verilog keyword?"}
  C -->|yes| E
  C -->|no| P["plain name"]
```
