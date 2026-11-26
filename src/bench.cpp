// SPDX-License-Identifier: Apache-2.0
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "rith/io.hpp"

namespace rith {

namespace {
std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> split_args(std::string_view s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const auto comma = s.find(',', start);
    const auto piece = trim(s.substr(start, comma == std::string_view::npos ? s.npos : comma - start));
    if (!piece.empty()) out.push_back(piece);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}
}  // namespace

Netlist read_bench(std::istream& in, const std::string& filename) {
  Netlist nl;
  std::vector<std::pair<std::string, int>> outputs;  // resolved after all gates are seen
  std::string raw;
  int lineno = 0;
  while (std::getline(in, raw)) {
    ++lineno;
    std::string line = trim(raw.substr(0, raw.find('#')));
    if (line.empty()) continue;
    const auto lp = line.find('('), rp = line.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp < lp)
      throw ParseError(filename, lineno, "expected NAME(...) in: " + line);
    const auto eq = line.find('=');
    try {
      if (eq == std::string::npos || eq > lp) {
        const std::string kw = trim(line.substr(0, lp));
        const std::string arg = trim(line.substr(lp + 1, rp - lp - 1));
        if (arg.empty()) throw ParseError(filename, lineno, "empty name in " + kw + "()");
        auto t = gate_type_from_string(kw);
        if (t == GateType::Input)
          nl.define(nl.get_or_declare(arg), GateType::Input);
        else if (kw == "OUTPUT" || kw == "output")
          outputs.emplace_back(arg, lineno);
        else
          throw ParseError(filename, lineno, "expected INPUT(...) or OUTPUT(...)");
        continue;
      }
      const std::string lhs = trim(line.substr(0, eq));
      const std::string fn = trim(line.substr(eq + 1, lp - eq - 1));
      const auto type = gate_type_from_string(fn);
      if (!type || *type == GateType::Input)
        throw ParseError(filename, lineno, "unknown gate type '" + fn + "'");
      std::vector<GateId> fanins;
      for (const auto& a : split_args(std::string_view(line).substr(lp + 1, rp - lp - 1)))
        fanins.push_back(nl.get_or_declare(a));
      nl.define(nl.get_or_declare(lhs), *type, std::move(fanins));
    } catch (const NetlistError& e) {
      throw ParseError(filename, lineno, e.what());
    }
  }
  for (const auto& [name, line] : outputs) {
    const GateId d = nl.find(name);
    if (d == kNoGate) throw ParseError(filename, line, "output '" + name + "' is never driven");
    nl.add_output(name, d);
  }
  nl.finalize();
  return nl;
}

void write_bench(std::ostream& out, const Netlist& nl) {
  out << "# " << nl.name << "\n# written by rithereal\n";
  out << "# " << nl.inputs().size() << " inputs, " << nl.outputs().size() << " outputs, "
      << nl.flops().size() << " flops, " << nl.size() << " gates\n\n";
  for (GateId g : nl.inputs()) out << "INPUT(" << nl.gate(g).name << ")\n";
  out << "\n";
  for (const auto& [name, d] : nl.outputs()) {
    if (name != nl.gate(d).name)
      throw NetlistError("BENCH cannot rename output '" + nl.gate(d).name + "' to '" + name + "'");
    out << "OUTPUT(" << name << ")\n";
  }
  out << "\n";
  for (GateId g : nl.definition_order()) {
    const Gate& gate = nl.gate(g);
    if (gate.type == GateType::Input) continue;
    out << gate.name << " = " << to_string(gate.type) << "(";
    bool first = true;
    for (GateId f : nl.fanins(g)) {
      out << (first ? "" : ", ") << nl.gate(f).name;
      first = false;
    }
    out << ")\n";
  }
}

}  // namespace rith
