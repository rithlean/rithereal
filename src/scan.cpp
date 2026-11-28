// SPDX-License-Identifier: Apache-2.0
#include "rith/scan.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "rith/io.hpp"
#include "rith/sim.hpp"
#include "libfunc.hpp"

namespace rith {

namespace {

using namespace libfunc;

std::size_t edit_distance(const std::string& a, const std::string& b) {
  std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

// Output pin carrying the state. An output the instance already uses is
// preferred, so stitching adds no new net (as Design Compiler does when only
// QN is connected); then non-inverting before inverting.
struct Tap {
  std::string pin;
  bool inverting = false;
};
std::optional<Tap> find_tap(const LibCell& c, const std::vector<std::string>* used = nullptr) {
  if (!c.flop) return std::nullopt;
  std::vector<std::string> vars = input_pins(c);
  vars.push_back(kState);
  std::optional<Tap> best;
  int best_rank = 4;
  for (const auto& p : c.pins) {
    if (p.dir != LibPin::Dir::Output) continue;
    const auto e = parse_opt(p.function);
    if (!e) continue;
    const LibFlop* ff = &*c.flop;
    const bool in_use = used && std::find(used->begin(), used->end(), p.name) != used->end();
    try {
      int rank = -1;
      if (for_all(vars, {}, [&](const Env& env) { return eval(*e, ff, env) == env.at(kState); })) rank = in_use ? 0 : 2;
      else if (for_all(vars, {}, [&](const Env& env) { return eval(*e, ff, env) != env.at(kState); }))
        rank = in_use ? 1 : 3;
      if (rank >= 0 && rank < best_rank) {
        best_rank = rank;
        best = Tap{p.name, rank % 2 == 1};
      }
    } catch (const std::exception&) {
    }
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scan cell recognition
// ---------------------------------------------------------------------------
std::optional<ScanPins> find_scan_pins(const LibCell& cell) {
  if (!cell.flop) return std::nullopt;
  const LibFlop& ff = *cell.flop;
  const auto ns = parse_opt(ff.next_state);
  if (!ns) return std::nullopt;

  static const std::set<std::string> kSiNames = {"SI", "SCD", "TI", "SDI", "SCAN_IN", "DFT_SDI", "SIN"};
  static const std::set<std::string> kSeNames = {"SE", "SCE", "TE", "SCAN_EN", "DFT_SDE", "SEN", "SEB", "TEN", "SEN_N"};
  std::vector<std::string> si_c, se_c;
  for (const auto& p : cell.pins) {
    if (p.dir != LibPin::Dir::Input) continue;
    if (p.signal_type == "test_scan_in" || p.signal_type == "test_scan_in_inverted") si_c.push_back(p.name);
    if (p.signal_type == "test_scan_enable" || p.signal_type == "test_scan_enable_inverted") se_c.push_back(p.name);
  }
  for (const auto& p : cell.pins) {
    if (p.dir != LibPin::Dir::Input) continue;
    if (si_c.empty() && kSiNames.count(upper(p.name))) si_c.push_back(p.name);
  }
  for (const auto& p : cell.pins) {
    if (p.dir != LibPin::Dir::Input) continue;
    if (se_c.empty() && kSeNames.count(upper(p.name))) se_c.push_back(p.name);
  }

  std::vector<std::string> vars = input_pins(cell);
  vars.push_back(kState);
  for (const auto& si : si_c)
    for (const auto& se : se_c) {
      if (si == se) continue;
      for (bool active : {true, false}) {
        std::vector<std::string> rest;
        for (const auto& v : vars)
          if (v != se) rest.push_back(v);
        try {
          // Scan mode: next state is the scan input.
          Env on;
          on[se] = active;
          const bool shifts = for_all(rest, on, [&](const Env& env) { return eval(*ns, &ff, env) == env.at(si); });
          if (!shifts) continue;
          // Mission mode: the scan input has no effect.
          Env off;
          off[se] = !active;
          const bool ignores = for_all(rest, off, [&](const Env& env) {
            Env flipped = env;
            flipped[si] = !env.at(si);
            return eval(*ns, &ff, env) == eval(*ns, &ff, flipped);
          });
          if (ignores) return ScanPins{si, se, active};
        } catch (const std::exception&) {
        }
      }
    }
  return std::nullopt;
}

std::optional<ScanEquivalent> find_scan_equivalent(const Library& lib, const LibCell& f,
                                                   const std::vector<std::string>* used_outputs) {
  if (!f.flop) return std::nullopt;
  if (auto own = find_scan_pins(f)) return ScanEquivalent{&f, *own};

  const LibFlop& ffa = *f.flop;
  const auto ns_f = parse_opt(ffa.next_state);
  const auto clk_f = parse_opt(ffa.clocked_on);
  if (!ns_f || !clk_f) return std::nullopt;
  const auto clr_f = parse_opt(ffa.clear), pre_f = parse_opt(ffa.preset);
  std::vector<std::string> f_in = input_pins(f);
  std::sort(f_in.begin(), f_in.end());
  std::vector<std::string> vars = f_in;
  vars.push_back(kState);

  std::vector<std::string> names;
  for (const auto& [n, c] : lib.cells) names.push_back(n);
  std::sort(names.begin(), names.end());

  std::optional<ScanEquivalent> best;
  std::size_t best_dist = 0;
  for (const auto& n : names) {
    const LibCell& s = lib.cells.at(n);
    if (!s.flop || &s == &f) continue;
    const auto sp = find_scan_pins(s);
    if (!sp) continue;
    std::vector<std::string> s_in;
    for (const auto& p : input_pins(s))
      if (p != sp->si && p != sp->se) s_in.push_back(p);
    std::sort(s_in.begin(), s_in.end());
    if (s_in != f_in) continue;

    const LibFlop& ffb = *s.flop;
    const auto ns_s = parse_opt(ffb.next_state), clk_s = parse_opt(ffb.clocked_on);
    const auto clr_s = parse_opt(ffb.clear), pre_s = parse_opt(ffb.preset);
    if (!ns_s || !clk_s || clr_f.has_value() != clr_s.has_value() || pre_f.has_value() != pre_s.has_value())
      continue;
    Env mission;
    mission[sp->se] = !sp->se_active_high;
    mission[sp->si] = false;
    bool same = true;
    try {
      auto equal = [&](const LibExpr& a, const LibExpr& b) {
        return for_all(vars, mission, [&](const Env& env) { return eval(a, &ffa, env) == eval(b, &ffb, env); });
      };
      same = equal(*ns_f, *ns_s) && equal(*clk_f, *clk_s) && (!clr_f || equal(*clr_f, *clr_s)) &&
             (!pre_f || equal(*pre_f, *pre_s));
      for (const auto& p : f.pins) {
        if (!same) break;
        if (p.dir != LibPin::Dir::Output) continue;
        if (used_outputs && std::find(used_outputs->begin(), used_outputs->end(), p.name) == used_outputs->end())
          continue;
        const LibPin* q = s.pin(p.name);
        const auto ef = parse_opt(p.function);
        const auto es = q ? parse_opt(q->function) : std::nullopt;
        same = q && q->dir == LibPin::Dir::Output && ef && es && equal(*ef, *es);
      }
    } catch (const std::exception&) {
      same = false;
    }
    if (!same) continue;
    const std::size_t dist = edit_distance(f.name, s.name);
    if (!best || dist < best_dist) {
      best = ScanEquivalent{&s, *sp};
      best_dist = dist;
    }
  }
  return best;
}

bool ScanChain::inverted() const {
  bool inv = false;
  for (const auto& c : cells) inv ^= c.inverting;
  return inv;
}

std::size_t ScanResult::scanned() const {
  std::size_t n = 0;
  for (const auto& c : chains) n += c.cells.size();
  return n;
}

// ---------------------------------------------------------------------------
// Insertion
// ---------------------------------------------------------------------------
namespace {

struct TypeInfo {
  std::string reason;  // empty when usable
  ScanEquivalent eq;
  Tap tap;
};

struct Domain {
  std::string clock;
  bool falling = false;
  std::vector<std::size_t> flops;  // instance indices, file order
  std::size_t chains = 1;
};

const LibCell* find_inverter(const Library& lib) {
  std::vector<const LibCell*> found;
  for (const auto& [n, c] : lib.cells) {
    if (c.flop || c.has_latch || c.has_statetable) continue;
    const auto ins = input_pins(c);
    std::vector<const LibPin*> outs;
    for (const auto& p : c.pins)
      if (p.dir == LibPin::Dir::Output) outs.push_back(&p);
    if (ins.size() != 1 || outs.size() != 1) continue;
    const auto e = parse_opt(outs[0]->function);
    if (!e) continue;
    try {
      if (eval(*e, nullptr, {{ins[0], false}}) && !eval(*e, nullptr, {{ins[0], true}})) found.push_back(&c);
    } catch (const std::exception&) {
    }
  }
  std::sort(found.begin(), found.end(), [](const LibCell* a, const LibCell* b) {
    const bool ia = upper(a->name).find("INV") != std::string::npos;
    const bool ib = upper(b->name).find("INV") != std::string::npos;
    if (ia != ib) return ia;
    return a->name < b->name;
  });
  return found.empty() ? nullptr : found[0];
}

}  // namespace

ScanResult insert_scan(Design& d, const Library& lib, const ScanConfig& cfg) {
  ScanResult r;
  const Elaboration el = elaborate(d, &lib);
  const Netlist& nl = el.netlist;

  // 1. Usable flops and their clock domains.
  std::map<std::string, TypeInfo> types;
  std::map<std::size_t, const TypeInfo*> type_of;  // instance -> its scan mapping
  std::map<std::pair<std::string, bool>, Domain> domains;
  for (std::size_t i = 0; i < d.instances.size(); ++i) {
    const GateId fg = el.flop_of_instance[i];
    if (fg == kNoGate) continue;
    const Instance& inst = d.instances[i];
    const LibCell* lc = lib.find(inst.type);
    if (!lc) {
      r.excluded.emplace_back(inst.name, "generic flop '" + inst.type + "'; map the design to a cell library first");
      continue;
    }
    // Only the outputs this instance uses have to exist on the scan cell.
    std::vector<std::string> used;
    for (const auto& c : inst.conns)
      if (c.net != kNoNet)
        if (const LibPin* p = lc->pin(c.pin); p && p->dir == LibPin::Dir::Output) used.push_back(c.pin);
    std::sort(used.begin(), used.end());
    std::string key = inst.type;
    for (const auto& u : used) key += " " + u;
    auto ti = types.find(key);
    if (ti == types.end()) {
      TypeInfo info;
      if (auto eq = find_scan_equivalent(lib, *lc, &used)) {
        info.eq = *eq;
        if (auto tap = find_tap(*eq->cell, &used)) info.tap = *tap;
        else info.reason = "scan cell '" + eq->cell->name + "' has no output carrying its state";
      } else {
        info.reason = "no scan equivalent of cell '" + inst.type + "' in the library";
      }
      ti = types.emplace(key, std::move(info)).first;
    }
    if (!ti->second.reason.empty()) {
      r.excluded.emplace_back(inst.name, ti->second.reason);
      continue;
    }
    // Trace the clock back through buffers and inverters to an input port.
    GateId c = nl.gate(fg).clock;
    bool falling = nl.gate(fg).falling_edge;
    if (c == kNoGate) {
      r.excluded.emplace_back(inst.name, "clock pin is not connected");
      continue;
    }
    while (nl.gate(c).type == GateType::Buf || nl.gate(c).type == GateType::Not) {
      if (nl.gate(c).type == GateType::Not) falling = !falling;
      c = nl.fanins(c)[0];
    }
    if (nl.gate(c).type != GateType::Input) {
      r.excluded.emplace_back(inst.name, "clock is not driven from an input port (gated or generated clock)");
      continue;
    }
    Domain& dom = domains[{nl.gate(c).name, falling}];
    dom.clock = nl.gate(c).name;
    dom.falling = falling;
    dom.flops.push_back(i);
    type_of[i] = &ti->second;
  }
  if (domains.empty()) {
    r.warnings.push_back("no flops can be put on a scan chain");
    return r;
  }

  // 2. How many chains each domain gets.
  std::vector<Domain*> doms;
  for (auto& [k, dm] : domains) doms.push_back(&dm);
  if (cfg.chains) {
    if (cfg.chains < doms.size())
      throw std::runtime_error("asked for " + std::to_string(cfg.chains) + " chain(s) but the design has " +
                               std::to_string(doms.size()) +
                               " clock domains (clock and edge); each needs its own chain");
    std::size_t extra = cfg.chains - doms.size();
    while (extra) {
      Domain* pick = nullptr;
      std::size_t worst = 0;
      for (Domain* dm : doms) {
        if (dm->chains >= dm->flops.size()) continue;
        const std::size_t len = (dm->flops.size() + dm->chains - 1) / dm->chains;
        if (len > worst) { worst = len; pick = dm; }
      }
      if (!pick) {
        r.warnings.push_back("fewer flops than requested chains; made " +
                             std::to_string(cfg.chains - extra) + " chains");
        break;
      }
      ++pick->chains;
      --extra;
    }
  } else if (cfg.max_length) {
    for (Domain* dm : doms) dm->chains = (dm->flops.size() + cfg.max_length - 1) / cfg.max_length;
  }

  // 3. Ports.
  auto check_free = [&](const std::string& name, const char* option) {
    if (d.find_port(name) || d.find_net(name) != kNoNet)
      throw std::runtime_error("'" + name + "' is already used in the design; choose another name with " + option);
  };
  std::size_t total = 0;
  for (Domain* dm : doms) total += dm->chains;
  for (std::size_t k = 1; k <= total; ++k) {
    check_free(cfg.si_prefix + std::to_string(k), "--si-prefix");
    check_free(cfg.so_prefix + std::to_string(k), "--so-prefix");
  }
  NetId se_net;
  if (const PortDecl* p = d.find_port(cfg.se_port)) {
    if (p->dir != PortDir::Input || p->range)
      throw std::runtime_error("'" + cfg.se_port + "' exists but is not a 1-bit input; choose another name with --se");
    se_net = d.find_net(cfg.se_port);
    r.warnings.push_back("reusing existing input port '" + cfg.se_port + "' as scan enable");
  } else {
    check_free(cfg.se_port, "--se");
    se_net = d.add_port(cfg.se_port, PortDir::Input);
  }
  r.se_port = cfg.se_port;
  NetId se_n = kNoNet;  // inverted scan enable, made on first use
  auto se_for = [&](bool active_high) {
    if (active_high) return se_net;
    if (se_n == kNoNet) {
      const LibCell* inv = find_inverter(lib);
      if (!inv) throw std::runtime_error("a scan cell needs an active-low scan enable but the library has no inverter");
      se_n = d.fresh_net(cfg.se_port + "_n");
      Instance i;
      i.type = inv->name;
      i.name = d.fresh_instance_name("rith_se_inv");
      for (const auto& p : inv->pins) i.conns.push_back({p.name, p.dir == LibPin::Dir::Input ? se_net : se_n});
      d.add_instance(std::move(i));
    }
    return se_n;
  };

  std::set<std::string> input_bits;
  for (const auto& p : d.ports)
    if (p.dir == PortDir::Input)
      for (const auto& b : d.port_bits(p)) input_bits.insert(b);

  // 4. Cut domains into balanced chains and stitch them.
  std::size_t chain_no = 0;
  for (Domain* dm : doms) {
    const std::size_t n = dm->flops.size(), c = dm->chains;
    std::size_t at = 0;
    for (std::size_t k = 0; k < c; ++k) {
      const std::size_t len = n / c + (k < n % c ? 1 : 0);
      ScanChain ch;
      ++chain_no;
      ch.name = "chain" + std::to_string(chain_no);
      ch.si_port = cfg.si_prefix + std::to_string(chain_no);
      ch.so_port = cfg.so_prefix + std::to_string(chain_no);
      ch.clock = dm->clock;
      ch.falling_edge = dm->falling;
      NetId prev = d.add_port(ch.si_port, PortDir::Input);
      for (std::size_t j = 0; j < len; ++j, ++at) {
        const std::size_t idx = dm->flops[at];
        const TypeInfo& ti = *type_of.at(idx);
        Instance& inst = d.instances[idx];
        if (ti.eq.cell->name == inst.type) {
          ++r.restitched;
          const NetId old_se = inst.net_on(ti.eq.pins.se);
          const bool inactive = !ti.eq.pins.se_active_high;
          if (old_se != kNoNet && !is_constant(old_se) && old_se != se_net) {
            if (input_bits.count(d.net_name(old_se))) r.old_scan_enables[d.net_name(old_se)] = inactive;
            else
              r.warnings.push_back("scan enable of '" + inst.name + "' was driven by logic ('" +
                                   d.net_name(old_se) + "'); it is now " + cfg.se_port);
          }
        } else {
          r.replaced_cells[inst.type] = ti.eq.cell->name;
          inst.type = ti.eq.cell->name;
          ++r.replaced;
        }
        inst.connect(ti.eq.pins.si, prev);
        inst.connect(ti.eq.pins.se, se_for(ti.eq.pins.se_active_high));
        NetId out = inst.net_on(ti.tap.pin);
        if (out == kNoNet) {
          out = d.fresh_net("rith_scan_" + std::to_string(chain_no) + "_" + std::to_string(j));
          inst.connect(ti.tap.pin, out);
        }
        ch.cells.push_back({idx, inst.name, ti.eq.pins.si, ti.tap.pin, ti.tap.inverting});
        prev = out;
      }
      const NetId so = d.add_port(ch.so_port, PortDir::Output);
      d.add_assign({so, prev, false, 0});
      r.chains.push_back(std::move(ch));
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// Verification by simulation
// ---------------------------------------------------------------------------
ScanCheck verify_mission_mode(const Elaboration& before, const Elaboration& after, const ScanResult& r,
                              std::size_t blocks, std::uint64_t seed) {
  ScanCheck res;
  const Netlist& a = before.netlist;
  const Netlist& b = after.netlist;
  std::unordered_map<std::string, std::size_t> a_in;
  for (std::size_t i = 0; i < a.inputs().size(); ++i) a_in.emplace(a.gate(a.inputs()[i]).name, i);
  std::unordered_map<GateId, std::size_t> a_ff, b_ff;
  for (std::size_t i = 0; i < a.flops().size(); ++i) a_ff.emplace(a.flops()[i], i);
  for (std::size_t i = 0; i < b.flops().size(); ++i) b_ff.emplace(b.flops()[i], i);

  // Flop pairs by instance.
  std::vector<std::pair<GateId, GateId>> flops;
  std::vector<long> b_state_src(b.flops().size(), -1);
  for (std::size_t i = 0; i < before.flop_of_instance.size(); ++i) {
    const GateId fa = before.flop_of_instance[i];
    if (fa == kNoGate) continue;
    if (i >= after.flop_of_instance.size() || after.flop_of_instance[i] == kNoGate) {
      res.ok = false;
      res.message = "flop '" + a.gate(fa).name + "' is missing after scan insertion";
      return res;
    }
    const GateId fb = after.flop_of_instance[i];
    flops.emplace_back(fa, fb);
    b_state_src[b_ff.at(fb)] = static_cast<long>(a_ff.at(fa));
  }
  struct Obs { std::string what; GateId ga, gb; };
  std::vector<Obs> obs;
  for (const auto& [name, da] : a.outputs()) {
    GateId db = kNoGate;
    for (const auto& [nb, g] : b.outputs())
      if (nb == name) db = g;
    if (db == kNoGate) {
      res.ok = false;
      res.message = "output '" + name + "' is missing after scan insertion";
      return res;
    }
    obs.push_back({"output " + name, da, db});
  }
  for (const auto& [fa, fb] : flops)
    obs.push_back({"next state of flop " + a.gate(fa).name, a.fanins(fa)[0], b.fanins(fb)[0]});

  std::mt19937_64 rng(seed);
  std::vector<Word> pa(a.inputs().size()), sa(a.flops().size()), pb(b.inputs().size()), sb(b.flops().size());
  for (std::size_t blk = 0; blk < blocks; ++blk) {
    for (auto& w : pa) w = rng();
    for (auto& w : sa) w = rng();
    for (std::size_t i = 0; i < pa.size(); ++i) {  // old scan enables stay off
      const std::string& n = a.gate(a.inputs()[i]).name;
      if (auto it = r.old_scan_enables.find(n); it != r.old_scan_enables.end()) pa[i] = it->second ? kAllOnes : 0;
      else if (n == r.se_port) pa[i] = 0;  // a reused scan enable is off in both designs
    }
    for (std::size_t i = 0; i < pb.size(); ++i) {
      const std::string& n = b.gate(b.inputs()[i]).name;
      if (n == r.se_port) pb[i] = 0;  // mission mode
      else if (auto it = a_in.find(n); it != a_in.end()) pb[i] = pa[it->second];
      else pb[i] = rng();  // scan-in ports: must not matter
    }
    for (std::size_t i = 0; i < sb.size(); ++i) sb[i] = b_state_src[i] >= 0 ? sa[b_state_src[i]] : rng();
    const auto va = simulate(a, pa, sa);
    const auto vb = simulate(b, pb, sb);
    res.patterns += 64;
    for (const auto& o : obs)
      if (va[o.ga] != vb[o.gb]) {
        res.ok = false;
        res.message = "mission mode differs at " + o.what;
        return res;
      }
  }
  return res;
}

ScanCheck verify_shift(const Elaboration& after, const ScanResult& r, std::uint64_t seed) {
  ScanCheck res;
  const Netlist& nl = after.netlist;
  std::unordered_map<std::string, std::size_t> in_idx;
  for (std::size_t i = 0; i < nl.inputs().size(); ++i) in_idx.emplace(nl.gate(nl.inputs()[i]).name, i);
  std::unordered_map<GateId, std::size_t> ff_idx;
  for (std::size_t i = 0; i < nl.flops().size(); ++i) ff_idx.emplace(nl.flops()[i], i);
  auto output_gate = [&](const std::string& n) {
    for (const auto& [name, g] : nl.outputs())
      if (name == n) return g;
    throw std::runtime_error("scan-out port '" + n + "' not found");
  };
  if (!in_idx.count(r.se_port)) {
    res.ok = false;
    res.message = "scan enable port '" + r.se_port + "' not found";
    return res;
  }

  struct ChainSim {
    std::size_t si;
    GateId so;
    std::vector<std::size_t> flop;  // index into nl.flops()
    std::vector<bool> parity;       // inversion seen before each cell
    bool total_parity = false;
    std::vector<Word> hist;
  };
  std::vector<ChainSim> cs;
  std::size_t longest = 0;
  for (const auto& ch : r.chains) {
    ChainSim s;
    s.si = in_idx.at(ch.si_port);
    s.so = output_gate(ch.so_port);
    bool par = false;
    for (const auto& c : ch.cells) {
      s.flop.push_back(ff_idx.at(after.flop_of_instance.at(c.instance)));
      s.parity.push_back(par);
      par ^= c.inverting;
    }
    s.total_parity = par;
    longest = std::max(longest, ch.cells.size());
    cs.push_back(std::move(s));
  }

  std::mt19937_64 rng(seed);
  std::vector<Word> pi(nl.inputs().size()), st(nl.flops().size());
  for (auto& w : st) w = rng();
  const std::size_t cycles = longest + 3;
  for (std::size_t t = 0; t < cycles; ++t) {
    for (auto& w : pi) w = rng();
    pi[in_idx.at(r.se_port)] = kAllOnes;
    for (auto& s : cs) s.hist.push_back(pi[s.si]);
    const auto v = simulate(nl, pi, st);
    for (std::size_t c = 0; c < cs.size(); ++c) {
      const auto& s = cs[c];
      for (std::size_t j = 0; j < s.flop.size(); ++j) {
        if (t < j + 1) continue;
        const Word want = s.hist[t - 1 - j] ^ (s.parity[j] ? kAllOnes : 0);
        if (st[s.flop[j]] != want) {
          res.ok = false;
          res.message = r.chains[c].name + ": cell " + std::to_string(j) + " ('" + r.chains[c].cells[j].name +
                        "') does not hold the shifted data after " + std::to_string(t) + " clocks";
          return res;
        }
      }
      const std::size_t len = s.flop.size();
      if (t >= len) {
        const Word want = s.hist[t - len] ^ (s.total_parity ? kAllOnes : 0);
        if (v[s.so] != want) {
          res.ok = false;
          res.message = r.chains[c].name + ": scan-out " + r.chains[c].so_port + " does not match the scan-in data";
          return res;
        }
      }
    }
    std::vector<Word> next(st.size());
    for (std::size_t i = 0; i < st.size(); ++i) next[i] = v[nl.fanins(nl.flops()[i])[0]];
    st.swap(next);
  }
  res.patterns = 64;
  return res;
}

}  // namespace rith
