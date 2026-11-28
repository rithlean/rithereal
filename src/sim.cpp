// SPDX-License-Identifier: Apache-2.0
#include "rith/sim.hpp"

#include <algorithm>

#include "rith/eval.hpp"

#include <random>
#include <unordered_map>

namespace rith {


std::vector<Word> simulate(const Netlist& nl, std::span<const Word> pi_words,
                           std::span<const Word> state_words) {
  if (!nl.finalized()) throw NetlistError("simulate() requires finalize()");
  if (pi_words.size() != nl.inputs().size() || state_words.size() != nl.flops().size())
    throw NetlistError("simulate(): input or state word count does not match the netlist");
  std::vector<Word> v(nl.size(), 0);
  for (std::size_t i = 0; i < pi_words.size(); ++i) v[nl.inputs()[i]] = pi_words[i];
  for (std::size_t i = 0; i < state_words.size(); ++i) v[nl.flops()[i]] = state_words[i];
  for (GateId g : nl.topo_order()) {
    const GateType t = nl.gate(g).type;
    if (t == GateType::Input || t == GateType::Dff) continue;
    const auto fi = nl.fanins(g);
    v[g] = eval_gate(t, fi.size(), [&](std::size_t i) { return v[fi[i]]; });
  }
  return v;
}

EquivalenceResult compare_by_simulation(const Netlist& a, const Netlist& b, std::size_t blocks,
                                        std::uint64_t seed) {
  EquivalenceOptions opt;
  opt.blocks = blocks;
  opt.seed = seed;
  return compare_by_simulation(a, b, opt);
}

EquivalenceResult compare_by_simulation(const Netlist& a, const Netlist& b, const EquivalenceOptions& opt) {
  EquivalenceResult res;
  const std::size_t blocks = opt.blocks;
  auto may_be_unmatched = [&](const Netlist& nl, GateId g) {
    const std::string& n = nl.gate(g).name;
    return nl.fanouts(g).empty() || opt.tie.count(n) || opt.ignore.count(n);
  };

  // Map b's inputs and flops to a's by name.
  auto index_of = [](const Netlist& nl, const std::vector<GateId>& ids) {
    std::unordered_map<std::string, std::size_t> m;
    for (std::size_t i = 0; i < ids.size(); ++i) m.emplace(nl.gate(ids[i]).name, i);
    return m;
  };
  const auto a_in = index_of(a, a.inputs());
  const auto a_ff = index_of(a, a.flops());

  std::vector<long> b_in_src(b.inputs().size(), -1);
  for (std::size_t i = 0; i < b.inputs().size(); ++i) {
    const GateId g = b.inputs()[i];
    if (auto it = a_in.find(b.gate(g).name); it != a_in.end())
      b_in_src[i] = static_cast<long>(it->second);
    else if (!may_be_unmatched(b, g))
      throw NetlistError("input '" + b.gate(g).name + "' exists only in the second netlist");
  }
  for (GateId g : a.inputs())
    if (b.find(a.gate(g).name) == kNoGate && !may_be_unmatched(a, g))
      throw NetlistError("input '" + a.gate(g).name + "' exists only in the first netlist");

  if (a.flops().size() != b.flops().size())
    throw NetlistError("flop counts differ: " + std::to_string(a.flops().size()) + " vs " +
                       std::to_string(b.flops().size()));
  std::vector<std::size_t> b_ff_src(b.flops().size());
  for (std::size_t i = 0; i < b.flops().size(); ++i) {
    auto it = a_ff.find(b.gate(b.flops()[i]).name);
    if (it == a_ff.end())
      throw NetlistError("flop '" + b.gate(b.flops()[i]).name + "' exists only in the second netlist");
    b_ff_src[i] = it->second;
  }

  // Observation points: outputs by name, and each flop's next state.
  struct Obs { std::string what; GateId ga, gb; };
  std::vector<Obs> obs;
  std::unordered_map<std::string, GateId> b_out;
  for (const auto& [nb, d] : b.outputs()) b_out.emplace(nb, d);
  for (const auto& [name, da] : a.outputs()) {
    if (opt.ignore.count(name)) continue;
    auto it = b_out.find(name);
    if (it == b_out.end()) throw NetlistError("output '" + name + "' exists only in the first netlist");
    obs.push_back({"output " + name, da, it->second});
  }
  for (const auto& [nb, d] : b.outputs())
    if (!opt.ignore.count(nb) && std::none_of(a.outputs().begin(), a.outputs().end(),
                                              [&](const auto& o) { return o.first == nb; }))
      throw NetlistError("output '" + nb + "' exists only in the second netlist");
  for (std::size_t i = 0; i < b.flops().size(); ++i) {
    const GateId fb = b.flops()[i], fa = a.flops()[b_ff_src[i]];
    obs.push_back({"next state of flop " + a.gate(fa).name, a.fanins(fa)[0], b.fanins(fb)[0]});
  }

  auto tie_of = [&](const Netlist& nl) {
    std::vector<int> t(nl.inputs().size(), -1);
    for (std::size_t i = 0; i < t.size(); ++i)
      if (auto it = opt.tie.find(nl.gate(nl.inputs()[i]).name); it != opt.tie.end()) t[i] = it->second;
    return t;
  };
  const auto a_tie = tie_of(a), b_tie = tie_of(b);

  std::mt19937_64 rng(opt.seed);
  std::vector<Word> pa(a.inputs().size()), sa(a.flops().size());
  std::vector<Word> pb(b.inputs().size()), sb(b.flops().size());
  for (std::size_t blk = 0; blk < blocks; ++blk) {
    for (std::size_t i = 0; i < pa.size(); ++i) pa[i] = a_tie[i] < 0 ? rng() : a_tie[i] ? kAllOnes : 0;
    for (auto& w : sa) w = rng();
    for (std::size_t i = 0; i < pb.size(); ++i)
      pb[i] = b_tie[i] >= 0 ? (b_tie[i] ? kAllOnes : 0) : b_in_src[i] >= 0 ? pa[b_in_src[i]] : rng();
    for (std::size_t i = 0; i < sb.size(); ++i) sb[i] = sa[b_ff_src[i]];
    const auto va = simulate(a, pa, sa);
    const auto vb = simulate(b, pb, sb);
    res.patterns += 64;
    for (const auto& o : obs) {
      if (va[o.ga] != vb[o.gb]) {
        res.equivalent = false;
        res.mismatch = o.what;
        return res;
      }
    }
  }
  return res;
}

}  // namespace rith
