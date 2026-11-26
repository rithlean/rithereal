// SPDX-License-Identifier: Apache-2.0
#include "rith/tpi.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "libfunc.hpp"
#include "rith/fault.hpp"
#include "rith/scan.hpp"

namespace rith {

std::string_view to_string(TpKind k) {
  switch (k) {
    case TpKind::Observe: return "observe";
    case TpKind::Control0: return "control-0";
    case TpKind::Control1: return "control-1";
  }
  return "?";
}

namespace {

constexpr double kQ = 0.5;  // probability a control point is active (its flop holds 1)

struct FullCop {
  std::vector<double> p1;    // original net
  std::vector<double> seen;  // what its gate and flop consumers see
  std::vector<double> obs;   // original net
};

FullCop cop_full(const Netlist& nl, std::span<const TestPoint> tps) {
  if (!nl.finalized()) throw NetlistError("test point analysis requires finalize()");
  const std::size_t n = nl.size();
  std::vector<std::uint8_t> kind(n, 0);  // bit 0 observe, bit 1 control-0, bit 2 control-1
  for (const auto& t : tps) {
    if (t.net >= n) throw NetlistError("test point on an unknown gate");
    kind[t.net] |= t.kind == TpKind::Observe ? 1 : t.kind == TpKind::Control0 ? 2 : 4;
  }
  FullCop c;
  c.p1.assign(n, 0.5);
  c.seen.assign(n, 0.5);
  c.obs.assign(n, 0.0);
  for (GateId g : nl.topo_order()) {
    const auto fi = nl.fanins(g);
    double p = 0.5;
    const GateType t = nl.gate(g).type;
    switch (t) {
      case GateType::Input:
      case GateType::Dff: p = 0.5; break;
      case GateType::Const0: p = 0.0; break;
      case GateType::Const1: p = 1.0; break;
      case GateType::Buf: p = c.seen[fi[0]]; break;
      case GateType::Not: p = 1.0 - c.seen[fi[0]]; break;
      case GateType::And:
      case GateType::Nand: {
        double q = 1.0;
        for (GateId f : fi) q *= c.seen[f];
        p = t == GateType::And ? q : 1.0 - q;
        break;
      }
      case GateType::Or:
      case GateType::Nor: {
        double q = 1.0;
        for (GateId f : fi) q *= 1.0 - c.seen[f];
        p = t == GateType::Or ? 1.0 - q : q;
        break;
      }
      case GateType::Xor:
      case GateType::Xnor: {
        double q = c.seen[fi[0]];
        for (std::size_t i = 1; i < fi.size(); ++i) q = q * (1.0 - c.seen[fi[i]]) + (1.0 - q) * c.seen[fi[i]];
        p = t == GateType::Xor ? q : 1.0 - q;
        break;
      }
      case GateType::Mux2: {
        const double s = c.seen[fi[0]];
        p = (1.0 - s) * c.seen[fi[1]] + s * c.seen[fi[2]];
        break;
      }
      case GateType::Undefined: break;
    }
    c.p1[g] = p;
    double sp = p;
    if (kind[g] & 2) sp = p * (1.0 - kQ);
    if (kind[g] & 4) sp = sp + (1.0 - sp) * kQ;
    c.seen[g] = sp;
  }

  // Observability. Consumers (gates, flop D) read the controlled value; outputs
  // and observe points read the original net.
  std::vector<double> not_seen(n, 1.0), not_raw(n, 1.0);
  for (const auto& [name, d] : nl.outputs()) not_raw[d] = 0.0;
  for (GateId f : nl.flops()) not_seen[nl.fanins(f)[0]] = 0.0;
  for (GateId g = 0; g < n; ++g)
    if (kind[g] & 1) not_raw[g] = 0.0;
  const auto& order = nl.topo_order();
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const GateId g = *it;
    double nr = not_raw[g];
    if (kind[g] & 6) {
      double through = 1.0 - not_seen[g];  // observability of the control gate output
      if (kind[g] & 2) through *= 1.0 - kQ;
      if (kind[g] & 4) through *= 1.0 - kQ;
      nr *= 1.0 - through;
    } else {
      nr *= not_seen[g];
    }
    c.obs[g] = 1.0 - nr;
    const GateType t = nl.gate(g).type;
    if (is_source(t)) continue;
    const double og = c.obs[g];
    if (og == 0.0) continue;
    const auto fi = nl.fanins(g);
    for (std::size_t p = 0; p < fi.size(); ++p) {
      double side = 1.0;
      switch (t) {
        case GateType::And:
        case GateType::Nand:
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side *= c.seen[fi[q]];
          break;
        case GateType::Or:
        case GateType::Nor:
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side *= 1.0 - c.seen[fi[q]];
          break;
        case GateType::Mux2:
          if (p == 0) {
            const double a = c.seen[fi[1]], b = c.seen[fi[2]];
            side = a * (1.0 - b) + (1.0 - a) * b;
          } else {
            side = p == 1 ? 1.0 - c.seen[fi[0]] : c.seen[fi[0]];
          }
          break;
        default: break;
      }
      not_seen[fi[p]] *= 1.0 - og * side;
    }
  }
  return c;
}

double escape(double pd, std::size_t patterns) {
  if (pd >= 1.0) return 0.0;
  if (pd <= 0.0) return 1.0;
  return std::exp(double(patterns) * std::log1p(-pd));
}

double gate_escape(GateType t, double p1, double obs, std::size_t patterns) {
  if (t == GateType::Const0 || t == GateType::Const1 || t == GateType::Undefined) return 0.0;
  return escape(p1 * obs, patterns) + escape((1.0 - p1) * obs, patterns);
}

double cost_of(const Netlist& nl, const FullCop& c, std::size_t patterns, const std::vector<char>& targets) {
  double s = 0;
  for (GateId g = 0; g < nl.size(); ++g)
    if (targets.empty() || targets[g]) s += gate_escape(nl.gate(g).type, c.p1[g], c.obs[g], patterns);
  return s;
}

// Gates from which some output or flop input can be reached.
std::vector<char> live_gates(const Netlist& nl) {
  std::vector<char> live(nl.size(), 0);
  std::vector<GateId> stack;
  auto mark = [&](GateId g) { if (!live[g]) { live[g] = 1; stack.push_back(g); } };
  for (const auto& [name, d] : nl.outputs()) mark(d);
  for (GateId f : nl.flops()) mark(nl.fanins(f)[0]);
  while (!stack.empty()) {
    const GateId g = stack.back();
    stack.pop_back();
    if (is_source(nl.gate(g).type)) continue;
    for (GateId f : nl.fanins(g)) mark(f);
  }
  return live;
}

}  // namespace

TpCop compute_cop_with(const Netlist& nl, std::span<const TestPoint> tps) {
  auto f = cop_full(nl, tps);
  return {std::move(f.p1), std::move(f.obs)};
}

double tpi_cost(const Netlist& nl, const TpCop& cop, std::size_t patterns) {
  double s = 0;
  for (GateId g = 0; g < nl.size(); ++g) s += gate_escape(nl.gate(g).type, cop.p1[g], cop.obs[g], patterns);
  return s;
}

// ---------------------------------------------------------------------------
// Greedy selection
// ---------------------------------------------------------------------------
TpiResult select_test_points(const Netlist& nl, const TpiConfig& cfg) {
  const std::size_t n = nl.size();
  const std::size_t N = cfg.patterns;
  const std::vector<char> live = live_gates(nl);
  std::vector<char> targets = live;  // faults the cost counts
  std::vector<char> clock_net(n, 0);
  for (GateId f : nl.flops())
    if (nl.gate(f).clock != kNoGate) clock_net[nl.gate(f).clock] = 1;
  auto eligible = [&](GateId g) {
    const GateType t = nl.gate(g).type;
    if (t == GateType::Input || t == GateType::Const0 || t == GateType::Const1 || t == GateType::Undefined)
      return false;
    if (clock_net[g] || !live[g]) return false;
    if (!cfg.allowed.empty() && (g >= cfg.allowed.size() || !cfg.allowed[g])) return false;
    return true;
  };

  TpiResult res;
  auto refine = [&] {
    if (!cfg.refine_every) return;
    const auto cov = random_coverage(nl, res.points, N, cfg.seed);
    std::fill(targets.begin(), targets.end(), 0);
    for (GateId g : cov.undetected_nets) targets[g] = live[g];
  };
  refine();
  FullCop cur = cop_full(nl, res.points);
  double cur_cost = cost_of(nl, cur, N, targets);
  res.cost.push_back(cur_cost);
  const unsigned threads = cfg.threads ? cfg.threads : std::max(1u, std::thread::hardware_concurrency());

  while (res.points.size() < cfg.max_points) {
    // Cheap scores.
    std::vector<double> esc(n);
    for (GateId g = 0; g < n; ++g)
      esc[g] = targets[g] ? gate_escape(nl.gate(g).type, cur.p1[g], cur.obs[g], N) : 0.0;
    std::vector<std::pair<double, TestPoint>> obs_c, c0, c1;
    std::set<std::pair<GateId, TpKind>> taken;
    for (const auto& t : res.points) taken.insert({t.net, t.kind});
    for (GateId g = 0; g < n; ++g) {
      if (!eligible(g)) continue;
      const GateType t = nl.gate(g).type;
      if (cfg.observe && !taken.count({g, TpKind::Observe}) && cur.obs[g] < 1.0) {
        // Local gain at the net, plus what its fanin cone stands to gain (rough).
        double gain = targets[g] ? esc[g] - gate_escape(t, cur.p1[g], 1.0, N) : 0.0;
        for (GateId f : nl.fanins(g)) gain += 0.5 * esc[f] * (1.0 - cur.obs[f]);
        if (gain > 0) obs_c.push_back({gain, {g, TpKind::Observe}});
      }
      if (cfg.control && t != GateType::Dff) {
        double down = 0;
        for (GateId c : nl.fanouts(g))
          if (nl.gate(c).type != GateType::Dff) down += esc[c];
        if (down > 0) {
          if (!taken.count({g, TpKind::Control0})) c0.push_back({down * cur.seen[g], {g, TpKind::Control0}});
          if (!taken.count({g, TpKind::Control1})) c1.push_back({down * (1.0 - cur.seen[g]), {g, TpKind::Control1}});
        }
      }
    }
    auto top = [](std::vector<std::pair<double, TestPoint>>& v, std::size_t k, std::vector<TestPoint>& out) {
      k = std::min(k, v.size());
      std::partial_sort(v.begin(), v.begin() + static_cast<long>(k), v.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second.net < b.second.net); });
      for (std::size_t i = 0; i < k; ++i) out.push_back(v[i].second);
    };
    std::vector<TestPoint> shortlist;
    const std::size_t m = std::max<std::size_t>(cfg.shortlist, 4);
    top(obs_c, cfg.control ? m / 2 : m, shortlist);
    top(c0, cfg.observe ? m / 4 : m / 2, shortlist);
    top(c1, cfg.observe ? m / 4 : m / 2, shortlist);
    if (shortlist.empty()) break;

    // Exact re-scoring, in parallel.
    std::vector<double> costs(shortlist.size());
    std::atomic<std::size_t> next{0};
    auto work = [&] {
      std::vector<TestPoint> tps = res.points;
      tps.push_back({});
      for (std::size_t i; (i = next++) < shortlist.size();) {
        tps.back() = shortlist[i];
        costs[i] = cost_of(nl, cop_full(nl, tps), N, targets);
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < std::min<unsigned>(threads, static_cast<unsigned>(shortlist.size())); ++t)
      pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();

    std::size_t best = 0;
    for (std::size_t i = 1; i < costs.size(); ++i)
      if (costs[i] < costs[best]) best = i;
    if (cur_cost - costs[best] < cfg.min_gain) break;
    res.points.push_back(shortlist[best]);
    if (cfg.refine_every && res.points.size() % cfg.refine_every == 0) refine();
    cur = cop_full(nl, res.points);
    cur_cost = cost_of(nl, cur, N, targets);
    res.cost.push_back(cur_cost);
  }
  return res;
}

// ---------------------------------------------------------------------------
// Test points as gates
// ---------------------------------------------------------------------------
Netlist apply_test_points(const Netlist& nl, std::span<const TestPoint> tps, const std::string& enable) {
  Netlist out;
  out.name = nl.name;
  out.warnings = nl.warnings;
  std::unordered_map<GateId, std::string> controlled;  // net -> name of the gate its consumers read
  for (std::size_t k = 0; k < tps.size(); ++k)
    if (tps[k].kind != TpKind::Observe) {
      if (controlled.count(tps[k].net)) throw NetlistError("two control points on one net");
      controlled[tps[k].net] = "rith_tp_c" + std::to_string(k);
    }
  auto check_free = [&](const std::string& name) {
    if (nl.find(name) != kNoGate) throw NetlistError("net name '" + name + "' is already used");
  };
  GateId clock = kNoGate;
  for (GateId f : nl.flops())
    if (nl.gate(f).clock != kNoGate) {
      clock = nl.gate(f).clock;
      break;
    }

  for (GateId g : nl.definition_order()) {
    const Gate& gate = nl.gate(g);
    std::vector<GateId> fi;
    for (GateId f : nl.fanins(g)) {
      auto it = controlled.find(f);
      fi.push_back(out.get_or_declare(it == controlled.end() ? nl.gate(f).name : it->second));
    }
    const GateId id = out.get_or_declare(gate.name);
    out.define(id, gate.type, std::move(fi));
    if (gate.type == GateType::Dff && gate.clock != kNoGate)
      out.set_clock(id, out.get_or_declare(nl.gate(gate.clock).name), gate.falling_edge);
  }
  GateId en;
  if (enable.empty()) {
    check_free("rith_tp_en");
    en = out.add_gate(GateType::Const1, "rith_tp_en");
  } else {
    check_free(enable);
    en = out.add_gate(GateType::Input, enable);
  }
  const GateId clk = clock == kNoGate ? kNoGate : out.get_or_declare(nl.gate(clock).name);
  for (std::size_t k = 0; k < tps.size(); ++k) {
    const auto& t = tps[k];
    const std::string ks = std::to_string(k);
    const GateId net = out.get_or_declare(nl.gate(t.net).name);
    if (t.kind == TpKind::Observe) {
      check_free("rith_tp_obs" + ks);
      const GateId f = out.add_gate(GateType::Dff, "rith_tp_obs" + ks, {net});
      if (clk != kNoGate) out.set_clock(f, clk);
      continue;
    }
    for (const char* p : {"rith_tp_ctl", "rith_tp_g", "rith_tp_c"}) check_free(p + ks);
    const GateId ctl = out.get_or_declare("rith_tp_ctl" + ks);
    out.define(ctl, GateType::Dff, {ctl});  // holds its scanned-in value
    if (clk != kNoGate) out.set_clock(ctl, clk);
    const bool zero = t.kind == TpKind::Control0;
    const GateId g = out.add_gate(zero ? GateType::Nand : GateType::And, "rith_tp_g" + ks, {en, ctl});
    out.define(out.get_or_declare("rith_tp_c" + ks), zero ? GateType::And : GateType::Or, {net, g});
  }
  for (const auto& [name, d] : nl.outputs()) out.add_output(name, out.get_or_declare(nl.gate(d).name));
  out.finalize();
  return out;
}

TpCoverage random_coverage(const Netlist& original, std::span<const TestPoint> tps, std::size_t patterns,
                           std::uint64_t seed, const std::string& enable) {
  const Netlist nl = tps.empty() ? original : apply_test_points(original, tps, enable);
  // The fault universe is the original netlist's, so coverage with and without
  // test points is measured over the same faults. A branch fault stays on the
  // same consumer pin, which a control point now feeds from the controlled net.
  const FaultList all = build_fault_list(original, false);
  std::vector<GateId> to(original.size());
  for (GateId g = 0; g < original.size(); ++g) to[g] = nl.find(original.gate(g).name);
  FaultList fl;
  for (Fault f : all.faults) {
    f.gate = to[f.gate];
    fl.faults.push_back(f);
    fl.weight.push_back(1);
  }
  fl.uncollapsed = fl.faults.size();
  const std::size_t unused = all.unused;
  std::vector<std::pair<std::size_t, bool>> fixed;
  if (!tps.empty())
    for (std::size_t i = 0; i < nl.inputs().size(); ++i)
      if (nl.gate(nl.inputs()[i]).name == enable) fixed.push_back({i, true});
  FaultSimulator sim(nl, std::move(fl));
  const auto r = run_random_patterns(sim, patterns, seed, 0, fixed);
  TpCoverage c;
  c.faults = r.faults;
  c.unused = unused;
  c.detected = r.detected;
  c.patterns = r.patterns;
  std::vector<char> seen(original.size(), 0);
  for (std::size_t i = 0; i < r.faults; ++i)
    if (sim.status(i) == FaultStatus::Undetected) {
      const Fault& f = all.faults[i];  // in original ids
      const GateId o = f.pin >= 0 ? original.fanins(f.gate)[static_cast<std::size_t>(f.pin)] : f.gate;
      if (!seen[o]) { seen[o] = 1; c.undetected_nets.push_back(o); }
    }
  return c;
}

// ---------------------------------------------------------------------------
// Test points in a mapped design
// ---------------------------------------------------------------------------
namespace {

using namespace libfunc;

struct PlainFlop {
  const LibCell* cell = nullptr;
  std::string d, clk, q;
};

PlainFlop find_plain_flop(const Library& lib) {
  std::vector<PlainFlop> found;
  for (const auto& [name, c] : lib.cells) {
    if (!c.flop || !c.flop->clear.empty() || !c.flop->preset.empty()) continue;
    const auto ins = input_pins(c);
    if (ins.size() != 2) continue;
    const auto ns = parse_opt(c.flop->next_state), clk = parse_opt(c.flop->clocked_on);
    if (!ns || !clk || ns->op != LibExpr::Op::Var || clk->op != LibExpr::Op::Var) continue;
    PlainFlop pf{&c, ns->var, clk->var, ""};
    for (const auto& p : c.pins)
      if (p.dir == LibPin::Dir::Output && p.function == c.flop->iq && pf.q.empty()) pf.q = p.name;
    if (pf.q.empty() || pf.d == pf.clk) continue;
    found.push_back(pf);
  }
  if (found.empty()) throw std::runtime_error("the library has no plain D flip-flop for test points");
  auto rank = [](const std::string& nm) {
    const std::string u = upper(nm);
    if (u.find("X1") != std::string::npos || u.find("_1") != std::string::npos) return 0;
    return 1;
  };
  std::sort(found.begin(), found.end(), [&](const PlainFlop& a, const PlainFlop& b) {
    if (rank(a.cell->name) != rank(b.cell->name)) return rank(a.cell->name) < rank(b.cell->name);
    if (a.cell->name.size() != b.cell->name.size()) return a.cell->name.size() < b.cell->name.size();
    return a.cell->name < b.cell->name;
  });
  return found[0];
}

// Whether connection k of an instance is an input of its cell.
bool is_input_conn(const Instance& inst, std::size_t k, const Library& lib) {
  if (inst.positional) return k > 0;
  if (const LibCell* c = lib.find(inst.type))
    if (const LibPin* p = c->pin(inst.conns[k].pin)) return p->dir == LibPin::Dir::Input;
  const std::string& pin = inst.conns[k].pin;
  return pin != "Y" && pin != "Q";  // Yosys generic cells and rith helpers
}

}  // namespace

TpDesignResult insert_test_points(Design& d, const Library& lib,
                                  const std::vector<std::pair<std::string, TpKind>>& points,
                                  const TpDesignConfig& cfg) {
  TpDesignResult r;
  if (points.empty()) return r;
  // Clock: the input port that clocks the most flops.
  r.clock = cfg.clock;
  if (r.clock.empty()) {
    const auto el = elaborate(d, &lib);
    const Netlist& nl = el.netlist;
    std::map<std::string, std::size_t> count;
    for (GateId f : nl.flops()) {
      GateId c = nl.gate(f).clock;
      if (c == kNoGate) continue;
      while (nl.gate(c).type == GateType::Buf || nl.gate(c).type == GateType::Not) c = nl.fanins(c)[0];
      if (nl.gate(c).type == GateType::Input) ++count[nl.gate(c).name];
    }
    std::size_t best = 0;
    for (const auto& [name, k] : count)
      if (k > best) { best = k; r.clock = name; }
    if (r.clock.empty()) throw std::runtime_error("cannot tell which clock to use for test point flops; pass --clock");
  }
  const NetId clk = d.find_net(r.clock);
  if (clk == kNoNet) throw std::runtime_error("clock '" + r.clock + "' is not a net of the design");
  if (d.find_port(cfg.enable) || d.find_net(cfg.enable) != kNoNet)
    throw std::runtime_error("'" + cfg.enable + "' is already used in the design; choose another name");
  const NetId en = d.add_port(cfg.enable, PortDir::Input);
  std::set<NetId> out_bits;
  for (const auto& p : d.ports)
    if (p.dir == PortDir::Output)
      for (const auto& b : d.port_bits(p)) out_bits.insert(d.find_net(b));

  const PlainFlop ff = find_plain_flop(lib);
  // AND/OR come from AND2/OR2 cells when the library has them, else from NAND2 and INV.
  struct Cell {
    const LibCell* cell = nullptr;
    std::vector<std::string> in;
    std::string out;
  };
  auto logic = [&](std::size_t n, std::uint64_t truth) {
    Cell c;
    c.cell = find_logic_cell(lib, n, truth, &c.in, &c.out);
    return c;
  };
  const Cell and2 = logic(2, 0b1000), nand2 = logic(2, 0b0111), or2 = logic(2, 0b1110), inv = logic(1, 0b01);
  auto add_cell = [&](const Cell& c, std::vector<NetId> ins, NetId y, const std::string& name) {
    Instance i;
    i.type = c.cell->name;
    i.name = d.fresh_instance_name(name);
    for (std::size_t j = 0; j < ins.size(); ++j) i.conns.push_back({c.in[j], ins[j]});
    i.conns.push_back({c.out, y});
    d.add_instance(std::move(i));
    ++r.gates;
  };
  auto add_nand = [&](NetId a, NetId b, NetId y, const std::string& name) { add_cell(nand2, {a, b}, y, name); };
  auto add_inv = [&](NetId a, NetId y, const std::string& name) { add_cell(inv, {a}, y, name); };
  auto add_and = [&](NetId a, NetId b, NetId y, const std::string& name) {
    if (and2.cell) return add_cell(and2, {a, b}, y, name);
    const NetId t = d.fresh_net(name + "_n");
    add_nand(a, b, t, name + "_nand");
    add_inv(t, y, name + "_inv");
  };
  auto add_flop = [&](NetId dn, NetId q, const std::string& name) {
    Instance i;
    i.type = ff.cell->name;
    i.name = d.fresh_instance_name(name);
    i.conns = {{ff.d, dn}, {ff.clk, clk}, {ff.q, q}};
    d.add_instance(std::move(i));
    ++r.flops;
  };

  for (std::size_t k = 0; k < points.size(); ++k) {
    const auto& [net_name, kind] = points[k];
    const NetId n = d.find_net(net_name);
    if (n == kNoNet) {
      r.warnings.push_back("net '" + net_name + "' not found; test point skipped");
      continue;
    }
    const std::string ks = std::to_string(k);
    if (kind == TpKind::Observe) {
      add_flop(n, d.fresh_net("rith_tp_obs" + ks + "_q"), "rith_tp_obs" + ks);
      continue;
    }
    const bool zero = kind == TpKind::Control0;
    if (!nand2.cell || (!inv.cell && !(zero ? and2.cell != nullptr : and2.cell && or2.cell)))
      throw std::runtime_error("the library lacks the NAND2 and INV (or AND2/OR2) cells control points need");
    const NetId c = d.fresh_net("rith_tp_c" + ks);
    // Every consumer of the net now reads the controlled net. Output ports keep
    // the original net, so only assigns to internal wires move.
    for (auto& inst : d.instances)
      for (std::size_t j = 0; j < inst.conns.size(); ++j)
        if (inst.conns[j].net == n && is_input_conn(inst, j, lib)) inst.conns[j].net = c;
    for (auto& a : d.assigns)
      if (a.rhs == n && !out_bits.count(a.lhs)) a.rhs = c;
    const NetId q = d.fresh_net("rith_tp_ctl" + ks + "_q");
    add_flop(q, q, "rith_tp_ctl" + ks);  // holds its scanned-in value
    const NetId g = d.fresh_net("rith_tp_g" + ks);
    if (zero) {  // c = n AND NOT(en AND q)
      add_nand(en, q, g, "rith_tp_nand" + ks);
      add_and(n, g, c, "rith_tp_and" + ks);
    } else if (or2.cell && (and2.cell || inv.cell)) {  // c = n OR (en AND q)
      add_and(en, q, g, "rith_tp_and" + ks);
      add_cell(or2, {n, g}, c, "rith_tp_or" + ks);
    } else {  // c = NAND(NOT n, NAND(en, q))
      const NetId nb = d.fresh_net("rith_tp_nb" + ks);
      add_nand(en, q, g, "rith_tp_nand" + ks);
      add_inv(n, nb, "rith_tp_inv" + ks);
      add_nand(nb, g, c, "rith_tp_or" + ks);
    }
  }
  return r;
}

}  // namespace rith
