// SPDX-License-Identifier: Apache-2.0
#include "rith/chains.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "rith/sim.hpp"

namespace rith {
namespace {

struct Src {
  enum Kind { None, Input, Flop } kind = None;
  std::size_t index = 0;
  bool inverted = false;
};

std::string key_of(const std::vector<Word>& w, bool invert) {
  std::string k(w.size() * sizeof(Word), '\0');
  for (std::size_t i = 0; i < w.size(); ++i) {
    const Word x = invert ? ~w[i] : w[i];
    for (std::size_t b = 0; b < sizeof(Word); ++b) k[i * sizeof(Word) + b] = static_cast<char>(x >> (8 * b));
  }
  return k;
}

bool looks_like_scan_out(const std::string& n) {
  std::string l = n;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return l.rfind("so", 0) == 0 || l.find("scan_out") != std::string::npos || l.find("test_so") != std::string::npos;
}

}  // namespace

TraceResult trace_scan_chains(const Netlist& nl, const TraceConfig& cfg) {
  const auto& ins = nl.inputs();
  const auto& ffs = nl.flops();
  long se = -1;
  std::vector<long> tied(ins.size(), -1);
  for (std::size_t i = 0; i < ins.size(); ++i) {
    const std::string& n = nl.gate(ins[i]).name;
    if (n == cfg.se_port) se = static_cast<long>(i);
    if (auto it = cfg.tie.find(n); it != cfg.tie.end()) tied[i] = it->second;
  }
  if (se < 0) throw std::runtime_error("scan enable '" + cfg.se_port + "' is not an input of the design");

  // Per-block words of every input, flop state, flop next state and output.
  const std::size_t k = std::max<std::size_t>(cfg.blocks, 1);
  std::vector<std::vector<Word>> in_sig(ins.size()), st_sig(ffs.size()), ns_sig(ffs.size()),
      out_sig(nl.outputs().size());
  std::mt19937_64 rng(cfg.seed);
  std::vector<Word> pi(ins.size()), st(ffs.size());
  for (std::size_t b = 0; b < k; ++b) {
    for (std::size_t i = 0; i < ins.size(); ++i)
      pi[i] = static_cast<long>(i) == se ? (cfg.se_active_high ? kAllOnes : 0)
              : tied[i] >= 0            ? (tied[i] ? kAllOnes : 0)
                                        : rng();
    for (auto& w : st) w = rng();
    const auto v = simulate(nl, pi, st);
    for (std::size_t i = 0; i < ins.size(); ++i) in_sig[i].push_back(pi[i]);
    for (std::size_t j = 0; j < ffs.size(); ++j) {
      st_sig[j].push_back(st[j]);
      ns_sig[j].push_back(v[nl.fanins(ffs[j])[0]]);
    }
    for (std::size_t o = 0; o < nl.outputs().size(); ++o) out_sig[o].push_back(v[nl.outputs()[o].second]);
  }

  // Sources by signature, in both polarities. Constant inputs are not sources.
  std::unordered_map<std::string, Src> by_sig;
  auto add = [&](const std::vector<Word>& sig, Src::Kind kind, std::size_t idx) {
    for (bool inv : {false, true}) {
      auto [it, fresh] = by_sig.emplace(key_of(sig, inv), Src{kind, idx, inv});
      if (!fresh) it->second.kind = Src::None;  // ambiguous: two sources look the same
    }
  };
  for (std::size_t i = 0; i < ins.size(); ++i)
    if (static_cast<long>(i) != se && tied[i] < 0) add(in_sig[i], Src::Input, i);
  for (std::size_t j = 0; j < ffs.size(); ++j) add(st_sig[j], Src::Flop, j);
  auto match = [&](const std::vector<Word>& sig) {
    auto it = by_sig.find(key_of(sig, false));
    return it == by_sig.end() ? Src{} : it->second;
  };

  std::vector<Src> pred(ffs.size());
  for (std::size_t j = 0; j < ffs.size(); ++j) pred[j] = match(ns_sig[j]);

  // Candidate chain ends: outputs that copy a flop, one output per flop,
  // preferring a scan-out-like name.
  std::unordered_map<std::size_t, std::pair<std::size_t, bool>> end_of;  // flop -> output, inverted
  for (std::size_t o = 0; o < nl.outputs().size(); ++o) {
    const Src s = match(out_sig[o]);
    if (s.kind != Src::Flop) continue;
    auto it = end_of.find(s.index);
    if (it == end_of.end()) end_of.emplace(s.index, std::make_pair(o, s.inverted));
    else if (!looks_like_scan_out(nl.outputs()[it->second.first].first) && looks_like_scan_out(nl.outputs()[o].first))
      it->second = {o, s.inverted};
  }
  std::vector<std::pair<std::size_t, std::pair<std::size_t, bool>>> ends(end_of.begin(), end_of.end());
  std::sort(ends.begin(), ends.end(), [](const auto& x, const auto& y) { return x.second.first < y.second.first; });

  // Walk each end back to a scan-in input.
  struct Walk {
    std::size_t out = 0;
    bool out_inv = false;
    std::vector<std::size_t> rev;  // last cell first
    std::vector<bool> link_inv;    // inversion from the predecessor into each cell
    Src head;
    bool ok = false;
  };
  std::vector<Walk> walks;
  std::vector<std::size_t> seen(ffs.size(), 0);
  std::size_t stamp = 0;
  for (const auto& [last, out] : ends) {
    Walk w;
    w.out = out.first;
    w.out_inv = out.second;
    ++stamp;
    bool loop = false;
    for (std::size_t f = last;;) {
      if (seen[f] == stamp) { loop = true; break; }
      seen[f] = stamp;
      w.rev.push_back(f);
      w.head = pred[f];
      w.link_inv.push_back(w.head.inverted);
      if (w.head.kind != Src::Flop) break;
      f = w.head.index;
    }
    w.ok = !loop && w.head.kind == Src::Input;
    if (!w.ok && !loop && w.rev.size() == 1) continue;  // a lone flop driving an output
    walks.push_back(std::move(w));
  }
  // An output that taps the middle of a longer chain is not a chain end.
  std::vector<char> inner(ffs.size(), 0);
  for (const auto& w : walks)
    if (w.ok)
      for (std::size_t i = 1; i < w.rev.size(); ++i) inner[w.rev[i]] = 1;

  TraceResult res;
  std::vector<char> on_chain(ffs.size(), 0);
  for (const auto& w : walks) {
    if (inner[w.rev[0]]) continue;
    if (!w.ok) {
      res.broken.push_back(nl.outputs()[w.out].first);
      continue;
    }
    TracedChain ch;
    ch.si_port = nl.gate(ins[w.head.index]).name;
    ch.so_port = nl.outputs()[w.out].first;
    bool par = false;
    for (std::size_t i = w.rev.size(); i-- > 0;) {
      par ^= w.link_inv[i];
      ch.cells.push_back({ffs[w.rev[i]], par});
      on_chain[w.rev[i]] = 1;
    }
    ch.so_inverted = par ^ w.out_inv;
    res.chains.push_back(std::move(ch));
  }
  for (std::size_t j = 0; j < ffs.size(); ++j)
    if (!on_chain[j]) res.off_chain.push_back(ffs[j]);
  return res;
}

}  // namespace rith
