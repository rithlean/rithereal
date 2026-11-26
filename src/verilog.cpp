// SPDX-License-Identifier: Apache-2.0
#include <cctype>
#include <functional>
#include <istream>
#include <iterator>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "rith/design.hpp"
#include "rith/io.hpp"

namespace rith {

namespace {

const std::unordered_set<std::string> kKeywords = {
    "always", "and", "assign", "begin", "buf", "case", "default", "else", "end", "endcase",
    "endmodule", "for", "function", "if", "initial", "inout", "input", "integer", "module",
    "nand", "negedge", "nor", "not", "or", "output", "parameter", "posedge", "reg", "supply0",
    "supply1", "task", "tri", "wire", "xnor", "xor", "logic", "generate", "localparam"};

}  // namespace

std::string verilog_identifier(std::string_view name) {
  bool simple = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
  for (char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) simple = false;
  if (simple && !kKeywords.count(std::string(name))) return std::string(name);
  return "\\" + std::string(name) + " ";
}

Netlist read_verilog(std::istream& in, const std::string& filename, const Library* lib) {
  return elaborate(read_verilog_design(in, filename), lib, filename).netlist;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void write_verilog(std::ostream& out, const Netlist& nl) {
  const auto id = [&](GateId g) { return verilog_identifier(nl.gate(g).name); };

  std::set<std::string> output_names;
  for (const auto& [name, d] : nl.outputs()) {
    if (nl.find(name) != kNoGate && nl.gate(nl.find(name)).type == GateType::Input)
      throw NetlistError("output '" + name + "' has the same name as an input");
    output_names.insert(name);
  }
  bool need_clk = false, uses_dff = false, uses_dffn = false, uses_mux = false;
  for (GateId f : nl.flops()) {
    (nl.gate(f).falling_edge ? uses_dffn : uses_dff) = true;
    if (nl.gate(f).clock == kNoGate) need_clk = true;
  }
  uses_mux = nl.count(GateType::Mux2) > 0;
  const std::string clk = "rith_clk";
  if (need_clk && nl.find(clk) != kNoGate) throw NetlistError("net name 'rith_clk' is reserved");

  out << "// written by rithereal\n";
  out << "module " << verilog_identifier(nl.name) << " (";
  bool first = true;
  auto port = [&](const std::string& n) { out << (first ? "" : ",") << "\n    " << n; first = false; };
  if (need_clk) port(clk);
  for (GateId g : nl.inputs()) port(id(g));
  for (const auto& [name, d] : nl.outputs()) port(verilog_identifier(name));
  out << "\n);\n";

  if (need_clk) out << "  input " << clk << ";\n";
  for (GateId g : nl.inputs()) out << "  input " << id(g) << ";\n";
  for (const auto& [name, d] : nl.outputs()) out << "  output " << verilog_identifier(name) << ";\n";
  for (GateId g : nl.definition_order())
    if (nl.gate(g).type != GateType::Input && !output_names.count(nl.gate(g).name))
      out << "  wire " << id(g) << ";\n";
  out << "\n";

  std::size_t inst_no = 0;
  for (GateId g : nl.definition_order()) {
    const Gate& gate = nl.gate(g);
    const auto fi = nl.fanins(g);
    if (gate.type == GateType::Input) continue;
    const std::string inst = "rith_i" + std::to_string(inst_no++);
    switch (gate.type) {
      case GateType::Input: break;
      case GateType::Const0: out << "  assign " << id(g) << " = 1'b0;\n"; break;
      case GateType::Const1: out << "  assign " << id(g) << " = 1'b1;\n"; break;
      case GateType::Mux2:
        out << "  rith_mux2 " << inst << " (.S(" << id(fi[0]) << "), .A(" << id(fi[1]) << "), .B("
            << id(fi[2]) << "), .Y(" << id(g) << "));\n";
        break;
      case GateType::Dff:
        out << (gate.falling_edge ? "  rith_dffn " : "  rith_dff ") << inst << " (.C(" << (gate.clock == kNoGate ? clk : id(gate.clock))
            << "), .D(" << id(fi[0]) << "), .Q(" << id(g) << "));\n";
        break;
      default: {
        std::string prim(to_string(gate.type));
        for (auto& c : prim) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out << "  " << prim << " " << inst << " (" << id(g);
        for (GateId f : fi) out << ", " << id(f);
        out << ");\n";
      }
    }
  }
  for (const auto& [name, d] : nl.outputs())
    if (name != nl.gate(d).name) out << "  assign " << verilog_identifier(name) << " = " << id(d) << ";\n";
  out << "endmodule\n";

  if (uses_dff)
    out << "\nmodule rith_dff (input C, input D, output reg Q);\n"
           "  always @(posedge C) Q <= D;\nendmodule\n";
  if (uses_dffn)
    out << "\nmodule rith_dffn (input C, input D, output reg Q);\n"
           "  always @(negedge C) Q <= D;\nendmodule\n";
  if (uses_mux)
    out << "\nmodule rith_mux2 (input S, input A, input B, output Y);\n"
           "  assign Y = S ? B : A;\nendmodule\n";
}

}  // namespace rith
