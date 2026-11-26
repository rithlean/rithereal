// SPDX-License-Identifier: Apache-2.0
#include "rith/netlist.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace rith {

namespace {
struct TypeName {
  GateType type;
  std::string_view name;
};
constexpr TypeName kNames[] = {
    {GateType::Undefined, "UNDEFINED"}, {GateType::Input, "INPUT"}, {GateType::Const0, "CONST0"},
    {GateType::Const1, "CONST1"},       {GateType::Buf, "BUF"},     {GateType::Not, "NOT"},
    {GateType::And, "AND"},             {GateType::Nand, "NAND"},   {GateType::Or, "OR"},
    {GateType::Nor, "NOR"},             {GateType::Xor, "XOR"},     {GateType::Xnor, "XNOR"},
    {GateType::Mux2, "MUX"},            {GateType::Dff, "DFF"},
};

std::string upper(std::string_view s) {
  std::string r(s);
  for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

// Allowed fanin counts per type: {min, max}.
std::pair<std::size_t, std::size_t> arity(GateType t) {
  switch (t) {
    case GateType::Input:
    case GateType::Const0:
    case GateType::Const1:
      return {0, 0};
    case GateType::Buf:
    case GateType::Not:
    case GateType::Dff:
      return {1, 1};
    case GateType::Mux2:
      return {3, 3};
    case GateType::And:
    case GateType::Nand:
    case GateType::Or:
    case GateType::Nor:
    case GateType::Xor:
    case GateType::Xnor:
      return {1, SIZE_MAX};
    case GateType::Undefined:
      break;
  }
  return {0, 0};
}
}  // namespace

std::string_view to_string(GateType t) {
  for (const auto& n : kNames)
    if (n.type == t) return n.name;
  return "?";
}

std::optional<GateType> gate_type_from_string(std::string_view s) {
  const std::string u = upper(s);
  if (u == "BUFF") return GateType::Buf;
  if (u == "INV") return GateType::Not;
  if (u == "MUX2") return GateType::Mux2;
  for (const auto& n : kNames)
    if (n.type != GateType::Undefined && n.name == u) return n.type;
  return std::nullopt;
}

bool is_source(GateType t) {
  return t == GateType::Input || t == GateType::Const0 || t == GateType::Const1 ||
         t == GateType::Dff;
}

GateId Netlist::find(std::string_view net) const {
  auto it = by_name_.find(std::string(net));
  return it == by_name_.end() ? kNoGate : it->second;
}

GateId Netlist::get_or_declare(std::string_view net) {
  if (finalized_) throw NetlistError("netlist is finalized; cannot add '" + std::string(net) + "'");
  if (GateId id = find(net); id != kNoGate) return id;
  const auto id = static_cast<GateId>(gates_.size());
  gates_.push_back(Gate{GateType::Undefined, std::string(net), kNoGate, false});
  build_fanins_.emplace_back();
  by_name_.emplace(std::string(net), id);
  return id;
}

void Netlist::define(GateId id, GateType type, std::vector<GateId> fanins) {
  if (finalized_) throw NetlistError("netlist is finalized");
  Gate& g = gates_.at(id);
  if (g.type != GateType::Undefined)
    throw NetlistError("net '" + g.name + "' has more than one driver");
  if (type == GateType::Undefined) throw NetlistError("cannot define a gate as UNDEFINED");
  const auto [lo, hi] = arity(type);
  if (fanins.size() < lo || fanins.size() > hi)
    throw NetlistError("gate '" + g.name + "' of type " + std::string(to_string(type)) + " has " +
                       std::to_string(fanins.size()) + " inputs");
  for (GateId f : fanins)
    if (f >= gates_.size()) throw NetlistError("gate '" + g.name + "' has an invalid fanin id");
  g.type = type;
  build_fanins_[id] = std::move(fanins);
  def_order_.push_back(id);
  if (type == GateType::Input) inputs_.push_back(id);
  if (type == GateType::Dff) flops_.push_back(id);
}

GateId Netlist::add_gate(GateType type, std::string_view net, std::vector<GateId> fanins) {
  if (find(net) != kNoGate) throw NetlistError("duplicate net net '" + std::string(net) + "'");
  GateId id = get_or_declare(net);
  define(id, type, std::move(fanins));
  return id;
}

void Netlist::set_clock(GateId flop, GateId clock, bool falling_edge) {
  Gate& g = gates_.at(flop);
  if (g.type != GateType::Dff) throw NetlistError("'" + g.name + "' is not a flop");
  g.clock = clock;
  g.falling_edge = falling_edge;
}

void Netlist::add_output(std::string port, GateId driver) {
  if (finalized_) throw NetlistError("netlist is finalized");
  for (const auto& [n, d] : outputs_)
    if (n == port) throw NetlistError("duplicate output '" + port + "'");
  outputs_.emplace_back(std::move(port), driver);
}

void Netlist::require_finalized(const char* what) const {
  if (!finalized_) throw NetlistError(std::string(what) + " requires finalize()");
}

std::span<const GateId> Netlist::fanins(GateId id) const {
  if (!finalized_) return build_fanins_.at(id);
  return {fanin_data_.data() + fanin_off_[id], fanin_off_[id + 1] - fanin_off_[id]};
}

std::span<const GateId> Netlist::fanouts(GateId id) const {
  require_finalized("fanouts()");
  return {fanout_data_.data() + fanout_off_[id], fanout_off_[id + 1] - fanout_off_[id]};
}

std::size_t Netlist::count(GateType t) const {
  return static_cast<std::size_t>(
      std::count_if(gates_.begin(), gates_.end(), [t](const Gate& g) { return g.type == t; }));
}

void Netlist::finalize() {
  if (finalized_) return;
  const std::size_t n = gates_.size();

  // 1. Every referenced net must be driven.
  std::string undriven;
  std::size_t undriven_count = 0;
  for (const auto& g : gates_) {
    if (g.type != GateType::Undefined) continue;
    if (undriven_count++ < 5) undriven += (undriven.empty() ? "" : ", ") + g.name;
  }
  if (undriven_count)
    throw NetlistError(std::to_string(undriven_count) + " undriven net(s): " + undriven +
                       (undriven_count > 5 ? ", ..." : ""));

  // 2. CSR fanins.
  fanin_off_.assign(n + 1, 0);
  for (std::size_t i = 0; i < n; ++i)
    fanin_off_[i + 1] = fanin_off_[i] + static_cast<std::uint32_t>(build_fanins_[i].size());
  fanin_data_.clear();
  fanin_data_.reserve(fanin_off_[n]);
  for (auto& fi : build_fanins_) fanin_data_.insert(fanin_data_.end(), fi.begin(), fi.end());

  // 3. CSR fanouts (all consumers, including flops).
  fanout_off_.assign(n + 1, 0);
  for (GateId f : fanin_data_) ++fanout_off_[f + 1];
  for (std::size_t i = 0; i < n; ++i) fanout_off_[i + 1] += fanout_off_[i];
  fanout_data_.assign(fanin_data_.size(), 0);
  {
    std::vector<std::uint32_t> cursor(fanout_off_.begin(), fanout_off_.end() - 1);
    for (std::size_t g = 0; g < n; ++g)
      for (GateId f : build_fanins_[g]) fanout_data_[cursor[f]++] = static_cast<GateId>(g);
  }
  build_fanins_.clear();
  build_fanins_.shrink_to_fit();
  finalized_ = true;

  // 4. Levelize the combinational view (flop inputs are cut).
  std::vector<std::uint32_t> pending(n, 0);
  topo_.clear();
  topo_.reserve(n);
  levels_.assign(n, 0);
  for (std::size_t g = 0; g < n; ++g) {
    if (is_source(gates_[g].type))
      topo_.push_back(static_cast<GateId>(g));
    else
      pending[g] = fanin_off_[g + 1] - fanin_off_[g];
  }
  for (std::size_t head = 0; head < topo_.size(); ++head) {
    const GateId u = topo_[head];
    for (GateId v : fanouts(u)) {
      if (is_source(gates_[v].type)) continue;
      levels_[v] = std::max(levels_[v], levels_[u] + 1);
      if (--pending[v] == 0) topo_.push_back(v);
    }
  }
  if (topo_.size() != n) {
    std::string loop;
    int shown = 0;
    for (std::size_t g = 0; g < n && shown < 5; ++g)
      if (pending[g]) loop += (shown++ ? ", " : "") + gates_[g].name;
    // The netlist is left unusable; callers should discard it.
    throw NetlistError("combinational loop through: " + loop);
  }
  depth_ = 0;
  for (auto l : levels_) depth_ = std::max(depth_, l);
}

}  // namespace rith
