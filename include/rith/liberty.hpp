// SPDX-License-Identifier: Apache-2.0
// Liberty (.lib) reader: just the logic view of each cell (pins, functions,
// flip-flops and scan attributes). Timing and power groups are skipped.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rith {

struct LibPin {
  enum class Dir { Input, Output, Inout, Internal };
  std::string name;
  Dir dir = Dir::Input;
  std::string function;     // Boolean function of an output pin, as written in the library
  std::string signal_type;  // e.g. test_scan_in, test_scan_enable (from the test_cell view)
};

struct LibFlop {
  std::string iq, iqn;  // internal state variable names, e.g. IQ and IQN
  std::string next_state, clocked_on;
  std::string clear, preset;  // asynchronous controls, empty when absent
};

struct LibCell {
  std::string name;
  std::vector<LibPin> pins;
  std::optional<LibFlop> flop;
  bool has_latch = false;      // latch group
  bool has_statetable = false; // statetable group (e.g. integrated clock gates)
  bool is_scan = false;        // has a test_cell group or scan signal_type pins

  const LibPin* pin(const std::string& n) const {
    for (const auto& p : pins)
      if (p.name == n) return &p;
    return nullptr;
  }
};

class Library {
 public:
  std::string name;
  std::unordered_map<std::string, LibCell> cells;
  const LibCell* find(const std::string& cell) const {
    auto it = cells.find(cell);
    return it == cells.end() ? nullptr : &it->second;
  }
};

Library read_liberty(std::istream& in, const std::string& filename = "<liberty>");
Library read_liberty_file(const std::string& path);
// Merges several libraries (later files win on duplicate cell names).
void merge_into(Library& dst, Library&& src);

// ---- Liberty Boolean expressions ------------------------------------------
// Precedence, high to low: ' and ! (NOT), ^ (XOR), & * or a space (AND), | + (OR).
struct LibExpr {
  enum class Op { Var, Const0, Const1, Not, And, Or, Xor } op = Op::Var;
  std::string var;
  std::vector<LibExpr> args;
};
LibExpr parse_liberty_function(const std::string& text);

}  // namespace rith
