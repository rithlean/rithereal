// SPDX-License-Identifier: Apache-2.0
// Netlist readers and writers.
#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include "rith/netlist.hpp"

namespace rith {

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& file, int line, const std::string& msg)
      : std::runtime_error(file + ":" + std::to_string(line) + ": " + msg) {}
};

// ISCAS BENCH format (ISCAS'85, ISCAS'89, ITC'99 conversions).
// Also accepts MUX(s, a0, a1), CONST0() and CONST1().
Netlist read_bench(std::istream& in, const std::string& filename = "<bench>");
void write_bench(std::ostream& out, const Netlist& nl);

// Flat structural Verilog. Reads one module made of:
//   * Verilog gate primitives (and, nand, or, nor, xor, xnor, not, buf)
//   * Yosys generic cells ($_AND_, $_OR_, $_NOT_, $_MUX_, $_DFF_P_, ...)
//   * rith_dff / rith_mux2 (what write_verilog emits)
//   * assign statements with a net, a constant or ~net on the right
// Bus declarations are expanded to bit nets named "bus[3]".
// Liberty-mapped cells (Design Compiler, Genus, Yosys + PDK) come next.
Netlist read_verilog(std::istream& in, const std::string& filename = "<verilog>");
void write_verilog(std::ostream& out, const Netlist& nl);

// Pick the format from the file extension (.bench or .v/.sv).
Netlist read_netlist_file(const std::string& path);
void write_netlist_file(const std::string& path, const Netlist& nl);

// Verilog identifier, escaped (\name followed by a space) when needed.
std::string verilog_identifier(std::string_view name);

}  // namespace rith
