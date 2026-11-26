// SPDX-License-Identifier: Apache-2.0
// Core netlist database: a flat gate graph with CSR fanin/fanout and levelization.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rith {

using GateId = std::uint32_t;
inline constexpr GateId kNoGate = 0xFFFFFFFFu;

// Every gate drives exactly one net, and the net carries the gate's name.
enum class GateType : std::uint8_t {
  Undefined,  // referenced but not (yet) driven; an error after finalize()
  Input,
  Const0,
  Const1,
  Buf,
  Not,
  And,
  Nand,
  Or,
  Nor,
  Xor,
  Xnor,
  Mux2,  // fanins: (select, in0, in1); out = select ? in1 : in0
  Dff,   // fanins: (d); clock kept separately in Gate::clock
};

std::string_view to_string(GateType t);
// Case-insensitive; accepts BENCH spellings such as BUFF.
std::optional<GateType> gate_type_from_string(std::string_view s);
// Sources of the combinational (full-scan) view: inputs, constants and flops.
bool is_source(GateType t);

struct Gate {
  GateType type = GateType::Undefined;
  std::string name;
  GateId clock = kNoGate;  // only for Dff
};

class NetlistError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Netlist {
 public:
  std::string name = "top";

  // ---- building -----------------------------------------------------------
  // Returns the gate with this name, creating an Undefined placeholder if needed.
  GateId get_or_declare(std::string_view net);
  // Gives a placeholder (or new gate) its type and fanins. Throws on a second driver.
  void define(GateId id, GateType type, std::vector<GateId> fanins = {});
  GateId add_gate(GateType type, std::string_view net, std::vector<GateId> fanins = {});
  void set_clock(GateId flop, GateId clock);
  void add_output(std::string port, GateId driver);
  // Validates arity and drivers, builds CSR fanin/fanout, topological order and levels.
  void finalize();

  // ---- queries ------------------------------------------------------------
  bool finalized() const { return finalized_; }
  std::size_t size() const { return gates_.size(); }
  const Gate& gate(GateId id) const { return gates_.at(id); }
  GateId find(std::string_view net) const;

  std::span<const GateId> fanins(GateId id) const;
  std::span<const GateId> fanouts(GateId id) const;  // requires finalize()

  const std::vector<GateId>& inputs() const { return inputs_; }
  const std::vector<std::pair<std::string, GateId>>& outputs() const { return outputs_; }
  const std::vector<GateId>& flops() const { return flops_; }
  // Gates in the order they were defined; writers use it so output is stable.
  const std::vector<GateId>& definition_order() const { return def_order_; }
  // Combinational evaluation order; sources first. Requires finalize().
  const std::vector<GateId>& topo_order() const { return topo_; }
  std::uint32_t level(GateId id) const { return levels_.at(id); }
  std::uint32_t depth() const { return depth_; }
  std::size_t count(GateType t) const;

 private:
  void require_finalized(const char* what) const;

  std::vector<Gate> gates_;
  std::vector<std::vector<GateId>> build_fanins_;
  std::unordered_map<std::string, GateId> by_name_;
  std::vector<GateId> inputs_;
  std::vector<GateId> flops_;
  std::vector<GateId> def_order_;
  std::vector<std::pair<std::string, GateId>> outputs_;

  bool finalized_ = false;
  std::vector<std::uint32_t> fanin_off_, fanout_off_;
  std::vector<GateId> fanin_data_, fanout_data_;
  std::vector<GateId> topo_;
  std::vector<std::uint32_t> levels_;
  std::uint32_t depth_ = 0;
};

}  // namespace rith
