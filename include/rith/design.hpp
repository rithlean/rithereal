// SPDX-License-Identifier: Apache-2.0
// Structural (cell-level) view of a flat Verilog netlist.
//
// A Design keeps the netlist as written: library cell instances with named pin
// connections, primitives, assign statements, ports and buses. Editing passes
// such as scan insertion work on it and write it back with the original cell
// names, so the result goes straight back into Design Compiler, Genus, Tessent
// or OpenROAD. Simulation and fault analysis use the gate-level Netlist that
// elaborate() derives from it.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rith/liberty.hpp"
#include "rith/netlist.hpp"

namespace rith {

using NetId = std::uint32_t;
inline constexpr NetId kNoNet = 0xFFFFFFFFu;      // unconnected pin
inline constexpr NetId kConst0Net = 0xFFFFFFFEu;  // 1'b0
inline constexpr NetId kConst1Net = 0xFFFFFFFDu;  // 1'b1
inline bool is_constant(NetId n) { return n == kConst0Net || n == kConst1Net; }

struct BitRange {
  long msb = 0, lsb = 0;
  std::vector<long> indices() const;  // msb first, as written
};

enum class PortDir { Input, Output };

struct PortDecl {
  std::string name;
  PortDir dir = PortDir::Input;
  std::optional<BitRange> range;  // vector port
};

struct PinConn {
  std::string pin;  // empty for positional (primitive) connections
  NetId net = kNoNet;
};

struct Instance {
  std::string type;  // cell or primitive name
  std::string name;  // may be empty for primitives
  bool positional = false;  // primitive: conns[0] is the output
  std::vector<PinConn> conns;
  int line = 0;  // source line, for messages

  // Net on a named pin; kNoNet when the pin is absent or left open.
  NetId net_on(std::string_view pin) const;
  // Connects (or reconnects) a named pin.
  void connect(std::string_view pin, NetId net);
};

struct Assign {
  NetId lhs = kNoNet;
  NetId rhs = kNoNet;  // a net or a constant
  bool invert = false; // assign lhs = ~rhs
  int line = 0;
};

class Design {
 public:
  std::string name;
  std::vector<PortDecl> ports;
  std::map<std::string, BitRange> buses;  // every vector declaration (ports and wires)
  std::vector<Instance> instances;
  std::vector<Assign> assigns;

  // Statement order as written: (false, i) is instances[i], (true, i) is assigns[i].
  const std::vector<std::pair<bool, std::size_t>>& order() const { return order_; }
  std::size_t add_instance(Instance inst);
  std::size_t add_assign(Assign a);

  // Nets are single bits; a bus bit is named "data[3]".
  NetId net(std::string_view name);  // finds or creates
  NetId find_net(std::string_view name) const;
  const std::string& net_name(NetId n) const { return nets_.at(n); }
  std::size_t net_count() const { return nets_.size(); }
  // A new net whose name starts with prefix and is not used yet.
  NetId fresh_net(std::string_view prefix);
  // An instance name that is not used yet.
  std::string fresh_instance_name(std::string_view prefix) const;

  // Bit names of a port, msb first ("a" or "a[3]", "a[2]", ...).
  std::vector<std::string> port_bits(const PortDecl& p) const;
  const PortDecl* find_port(std::string_view name) const;
  // Adds a scalar port and its net.
  NetId add_port(std::string port, PortDir dir);

 private:
  std::vector<std::string> nets_;
  std::unordered_map<std::string, NetId> index_;
  std::vector<std::pair<bool, std::size_t>> order_;
};

Design read_verilog_design(std::istream& in, const std::string& filename = "<verilog>");
Design read_verilog_design_file(const std::string& path);
// Writes the design back as flat structural Verilog with its original cell names.
void write_verilog_design(std::ostream& out, const Design& d);
void write_verilog_design_file(const std::string& path, const Design& d);

struct Elaboration {
  Netlist netlist;
  // For each instance: the Dff gate it became, or kNoGate if it is not a flop.
  std::vector<GateId> flop_of_instance;
};

// Expands the design into primitive gates (see read_verilog for what is
// understood). Throws ParseError naming the instance on anything unsupported.
Elaboration elaborate(const Design& d, const Library* lib = nullptr, const std::string& filename = "<verilog>");

}  // namespace rith
