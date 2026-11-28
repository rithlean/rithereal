# Liberty library reader

Files: `include/rith/liberty.hpp`, `src/liberty.cpp`, `src/libfunc.hpp`

[Back to the index](../README.md) · [Architecture](../architecture.md)

## What it is for

A Liberty file (`.lib`) is the chip factory's description of its cells. rithereal
reads only the **logic** part of it and skips timing and power:

- each pin's name, direction, Boolean `function` and `signal_type` (scan pins);
- the `ff` group of flip-flops (`next_state`, `clocked_on`, `clear`, `preset`);
- whether a cell has a `latch` or `statetable` group (not supported yet);
- whether a cell is a scan cell (`test_cell` group or `test_scan_*` signal types).

This lets the [Design elaborator](design.md) turn any library cell into primitive
gates, and lets [scan insertion](scan.md) and [TPI](tpi.md) find the cells they need
by what they do rather than by name.

## Reading a library

Reading happens in three steps: split the text into tokens, build a tree of groups,
then pick the cells out of the tree.

```mermaid
flowchart LR
  T[".lib text"] --> L["lex()<br/>tokens: words, strings,<br/>symbols { } ( ) : ; ,"]
  L --> P["GroupParser<br/>tree of Group:<br/>type, args, attributes, child groups"]
  P --> M["read_liberty:<br/>check top group is 'library';<br/>make_cell() for each 'cell' group"]
  M --> LIB["Library<br/>name + map of LibCell"]
```

### `lex()`

```mermaid
flowchart TD
  A(["lex(text)"]) --> B{"next character"}
  B -->|newline| B1["count the line"] --> B
  B -->|space| B
  B -->|"backslash before newline"| B2["line continuation: skip"] --> B
  B -->|"/* or //"| B3["skip the comment"] --> B
  B -->|"double quote"| S["read to the closing quote<br/>(continuations removed) = String"] --> B
  B -->|"{ } ( ) : ; ,"| Y["one-character Sym"] --> B
  B -->|word character| W["read a run of word characters = Word<br/>(letters, digits and _ . - + [ ] ! ' * & | ^ / $ < > = @)"] --> B
  B -->|other| X["ParseError: unexpected character"]
  B -->|end of text| Z(["tokens + End"])
```

Function operators such as `!`, `&`, `|` are word characters, so an unquoted
function stays in one piece.

### `GroupParser`

Liberty has three kinds of statement, and the parser tells them apart by what
follows the name:

```mermaid
flowchart TD
  A(["body(group)"]) --> B{"next token"}
  B -->|"}"| Z(["end of this group"])
  B -->|end of file| X["ParseError: missing '}'"]
  B -->|Word name| C{"after the name"}
  C -->|":"| S["simple attribute: name : value ;<br/>join words until ';' or '}'"] --> B
  C -->|"("| D["read argument list"]
  D --> E{"then '{'?"}
  E -->|yes| G["child group: recurse into body()"] --> B
  E -->|no| H["complex attribute: name(args);<br/>keep the first argument"] --> B
  C -->|other| X2["ParseError: expected ':' or '('"]
```

### `make_cell()`

```mermaid
flowchart TD
  A(["make_cell(cell group)"]) --> B["for each child group"]
  B --> C{"child type"}
  C -->|pin| P["one LibPin per name in pin(...):<br/>direction, function, signal_type;<br/>signal_type test_scan* marks is_scan"]
  C -->|ff| F["LibFlop: iq, iqn names,<br/>next_state, clocked_on, clear, preset"]
  C -->|latch| L["has_latch = true"]
  C -->|statetable| S["has_statetable = true"]
  C -->|test_cell| T["is_scan = true;<br/>copy signal_type from its pins<br/>to the cell's pins that lack one"]
  C -->|other: timing, power ...| B
  P & F & L & S & T --> B
  B -->|done| Z(["LibCell"])
```

`merge_into()` joins several libraries (from repeated `--lib` options); a later
file wins when two define the same cell name.

## Boolean functions (`parse_liberty_function`)

Cell functions such as `"!(A1 & A2) | B"` or `"(D&!SE) | (SI&SE)"` are parsed into
a small tree, `LibExpr`, by a recursive-descent parser. Operator strength, from
strongest to weakest:

| Strength | Operators | Meaning |
|---|---|---|
| 1 | `!x`, `x'` | NOT |
| 2 | `^` | XOR |
| 3 | `&`, `*`, or just a space (`A B`) | AND |
| 4 | `\|`, `+` | OR |

```mermaid
flowchart TD
  OR["parse_or:<br/>and-term { ('|' or '+') and-term }"] --> AND["parse_and:<br/>xor-term { ('&' or '*' or nothing) xor-term }"]
  AND --> XOR["parse_xor:<br/>unary { '^' unary }"]
  XOR --> UN["parse_unary:<br/>'!' unary, or primary followed by any number of '"]
  UN --> PR["parse_primary"]
  PR --> Q{"next character"}
  Q -->|"("| PAR["parse_or, then expect ')'"]
  Q -->|name| VAR["Var (or Const0 / Const1 for '0' / '1')"]
  Q -->|other| ERR["error: unexpected character"]
  PAR -.-> OR
```

`binary()` flattens chains, so `A & B & C` becomes one AND node with three
arguments instead of a nest of two-input nodes.

## `libfunc.hpp`: evaluating functions by truth table

This private header is shared by `scan.cpp` and `tpi.cpp`. It answers questions
like "do these two cells do the same thing?" by trying every combination of inputs.

| Function | What it does |
|---|---|
| `eval(expr, ff, env)` | Value of a function for one set of input values. The flop's state variable (for example `IQ`) is read from a shared name `@IQ`, so two different cells' states can be compared. `IQN` reads as NOT `@IQ`. |
| `for_all(vars, base, f)` | Calls `f` for every 0/1 assignment of `vars` (at most 16 variables, so at most 65,536 cases), stopping at the first `false`. |
| `input_pins(cell)` | Names of the cell's input pins, in order. |
| `parse_opt(text)` | Parses a function, or returns nothing if it is empty or invalid. |
| `find_logic_cell(lib, n, truth)` | Finds a one-output combinational cell with `n` inputs whose truth table equals `truth`, e.g. an AND2 is `0b1000`. |

```mermaid
flowchart TD
  A(["find_logic_cell(lib, n_inputs, truth)"]) --> B["for each cell in the library"]
  B --> C{"combinational, n inputs,<br/>one output with a function?"}
  C -->|no| B
  C -->|yes| D["for every input combination m:<br/>does eval(function) equal bit m of truth?"]
  D -->|all match| E["keep it"] --> B
  D -->|any differs| B
  B -->|done| F{"any kept?"}
  F -->|no| N(["nullptr"])
  F -->|yes| G["sort: smallest drive first (name has X0, then X1 or _1),<br/>then shorter name, then alphabetical"]
  G --> H(["first cell, with its input pin order and output pin"])
```
