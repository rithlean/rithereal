// SPDX-License-Identifier: Apache-2.0
#include "rith/atpg.hpp"

#include <algorithm>
#include <ostream>
#include <random>
#include <stdexcept>

#include "rith/eval.hpp"

namespace rith {

std::string_view to_string(AtpgOutcome o) {
  switch (o) {
    case AtpgOutcome::Detected: return "detected";
    case AtpgOutcome::Redundant: return "redundant";
    case AtpgOutcome::Aborted: return "aborted";
  }
  return "?";
}

namespace {

// Three-valued logic: 0, 1, X.
constexpr std::uint8_t k0 = 0, k1 = 1, kX = 2;

std::uint8_t v_not(std::uint8_t a) { return a == kX ? kX : std::uint8_t(a ^ 1); }

// Evaluates a gate in three-valued logic; in(i) gives the value on pin i.
template <class In>
std::uint8_t eval3(GateType t, std::size_t n, In&& in) {
  switch (t) {
    case GateType::Const0: return k0;
    case GateType::Const1: return k1;
    case GateType::Buf: return in(0);
    case GateType::Not: return v_not(in(0));
    case GateType::And:
    case GateType::Nand: {
      std::uint8_t r = k1;
      for (std::size_t i = 0; i < n; ++i) {
        const auto v = in(i);
        if (v == k0) { r = k0; break; }
        if (v == kX) r = kX;
      }
      return t == GateType::And ? r : v_not(r);
    }
    case GateType::Or:
    case GateType::Nor: {
      std::uint8_t r = k0;
      for (std::size_t i = 0; i < n; ++i) {
        const auto v = in(i);
        if (v == k1) { r = k1; break; }
        if (v == kX) r = kX;
      }
      return t == GateType::Or ? r : v_not(r);
    }
    case GateType::Xor:
    case GateType::Xnor: {
      std::uint8_t r = k0;
      for (std::size_t i = 0; i < n; ++i) {
        const auto v = in(i);
        if (v == kX) return kX;
        r ^= v;
      }
      return t == GateType::Xor ? r : v_not(r);
    }
    case GateType::Mux2: {
      const auto s = in(0), a = in(1), b = in(2);
      if (s == k0) return a;
      if (s == k1) return b;
      return a == b ? a : kX;
    }
    default: return kX;
  }
}

bool inverting(GateType t) { return t == GateType::Not || t == GateType::Nand || t == GateType::Nor || t == GateType::Xnor; }

const char* vname(std::uint8_t v) { return v == k0 ? "0" : v == k1 ? "1" : "X"; }

}  // namespace

struct Podem::Impl {
  const Netlist& nl;
  Scoap scoap;
  std::vector<std::size_t> source_index;  // gate -> position in the cube, or npos
  std::vector<GateId> sources;            // inputs then flops
  std::vector<char> is_obs;               // nets observed by an output or a flop D input
  std::vector<GateId> obs;
  std::vector<std::uint8_t> base;         // all sources X, constants implied
  // Per-fault state.
  std::vector<std::uint8_t> g, f;
  Fault fault;
  GateId site = kNoGate;  // net whose good value excites the fault
  std::uint8_t stuck = 0;
  std::vector<GateId> cone;  // gates the fault effect can reach, in level order
  std::vector<char> in_cone;
  std::vector<std::vector<GateId>> buckets;
  std::vector<char> queued;
  std::vector<std::uint32_t> stamp;
  std::uint32_t stamp_now = 0;
  std::vector<std::string>* trace = nullptr;

  explicit Impl(const Netlist& n) : nl(n), scoap(compute_scoap(n)) {
    const std::size_t N = nl.size();
    source_index.assign(N, SIZE_MAX);
    for (GateId i : nl.inputs()) { source_index[i] = sources.size(); sources.push_back(i); }
    for (GateId i : nl.flops()) { source_index[i] = sources.size(); sources.push_back(i); }
    is_obs.assign(N, 0);
    auto add_obs = [&](GateId d) { if (!is_obs[d]) { is_obs[d] = 1; obs.push_back(d); } };
    for (const auto& [name, d] : nl.outputs()) add_obs(d);
    for (GateId ff : nl.flops()) add_obs(nl.fanins(ff)[0]);
    base.assign(N, kX);
    for (GateId gid : nl.topo_order()) {
      const GateType t = nl.gate(gid).type;
      if (is_source(t)) continue;
      const auto fi = nl.fanins(gid);
      base[gid] = eval3(t, fi.size(), [&](std::size_t i) { return base[fi[i]]; });
    }
    buckets.resize(nl.depth() + 1);
    queued.assign(N, 0);
    in_cone.assign(N, 0);
    stamp.assign(N, 0);
  }

  // Faulty-machine value on pin i of gate gid (the fault may sit on that pin).
  std::uint8_t fpin(GateId gid, std::size_t i) const {
    if (fault.pin >= 0 && fault.gate == gid && static_cast<std::size_t>(fault.pin) == i) return stuck;
    return f[nl.fanins(gid)[i]];
  }
  bool effect_on_pin(GateId gid, std::size_t i) const {
    const std::uint8_t a = g[nl.fanins(gid)[i]], b = fpin(gid, i);
    return a != kX && b != kX && a != b;
  }
  bool effect_at(GateId n) const { return g[n] != kX && f[n] != kX && g[n] != f[n]; }

  void eval_gate(GateId gid) {
    const GateType t = nl.gate(gid).type;
    const auto fi = nl.fanins(gid);
    g[gid] = eval3(t, fi.size(), [&](std::size_t i) { return g[fi[i]]; });
    if (fault.pin < 0 && fault.gate == gid) f[gid] = stuck;
    else if (in_cone[gid]) f[gid] = eval3(t, fi.size(), [&](std::size_t i) { return fpin(gid, i); });
    else f[gid] = g[gid];
  }

  // Event-driven implication after the value of one source changed.
  void imply_from(GateId src) {
    std::size_t pending = 0;
    auto schedule = [&](GateId h) {
      for (GateId o : nl.fanouts(h))
        if (!queued[o] && !is_source(nl.gate(o).type)) {
          queued[o] = 1;
          buckets[nl.level(o)].push_back(o);
          ++pending;
        }
    };
    schedule(src);
    for (std::size_t lv = 0; pending && lv < buckets.size(); ++lv) {
      auto& b = buckets[lv];
      for (std::size_t k = 0; k < b.size(); ++k) {
        const GateId h = b[k];
        queued[h] = 0;
        --pending;
        const auto og = g[h], of = f[h];
        eval_gate(h);
        if (g[h] != og || f[h] != of) schedule(h);
      }
      b.clear();
    }
  }

  void assign(GateId src, std::uint8_t v) {
    g[src] = v;
    f[src] = (fault.pin < 0 && fault.gate == src) ? stuck : v;
    imply_from(src);
  }

  void setup(const Fault& flt) {
    fault = flt;
    stuck = flt.stuck_at_one ? k1 : k0;
    site = flt.pin < 0 ? flt.gate : nl.fanins(flt.gate)[static_cast<std::size_t>(flt.pin)];
    for (GateId h : cone) in_cone[h] = 0;
    cone.clear();
    // Fanout cone of the gate where the faulty machine first differs.
    std::vector<GateId> stack{flt.gate};
    in_cone[flt.gate] = 1;
    while (!stack.empty()) {
      const GateId h = stack.back();
      stack.pop_back();
      cone.push_back(h);
      for (GateId o : nl.fanouts(h))
        if (!in_cone[o] && !is_source(nl.gate(o).type)) { in_cone[o] = 1; stack.push_back(o); }
    }
    std::sort(cone.begin(), cone.end(), [&](GateId a, GateId b) {
      return nl.level(a) != nl.level(b) ? nl.level(a) < nl.level(b) : a < b;
    });
    g = base;
    f = base;
    // The faulty machine starts from all-X sources too; evaluate the cone once.
    for (GateId h : cone) {
      if (is_source(nl.gate(h).type)) {
        if (flt.pin < 0 && h == flt.gate) f[h] = stuck;
        continue;
      }
      eval_gate(h);
    }
  }

  bool detected() const {
    if (fault.pin >= 0 && nl.gate(fault.gate).type == GateType::Dff)  // a flop D pin is observed itself
      return g[site] != kX && g[site] != stuck;
    for (GateId o : obs)
      if (effect_at(o)) return true;
    return false;
  }

  // Gates whose output is still open (an X in either machine) with a fault
  // effect on an input.
  void d_frontier(std::vector<GateId>& out) const {
    out.clear();
    for (GateId h : cone) {
      if (is_source(nl.gate(h).type)) continue;
      if (g[h] != kX && f[h] != kX) continue;
      const std::size_t n = nl.fanins(h).size();
      for (std::size_t i = 0; i < n; ++i)
        if (effect_on_pin(h, i)) { out.push_back(h); break; }
    }
  }

  // Is there a path of open nets from h to an observed net?
  bool x_path(GateId h) {
    ++stamp_now;
    std::vector<GateId> stack{h};
    stamp[h] = stamp_now;
    while (!stack.empty()) {
      const GateId x = stack.back();
      stack.pop_back();
      if (is_obs[x]) return true;
      for (GateId o : nl.fanouts(x)) {
        if (stamp[o] == stamp_now || is_source(nl.gate(o).type)) continue;
        stamp[o] = stamp_now;
        if (g[o] != kX && f[o] != kX && g[o] == f[o]) continue;  // blocked
        stack.push_back(o);
      }
    }
    return false;
  }

  std::uint32_t cc(GateId n, std::uint8_t v) const { return v == k1 ? scoap.cc1[n] : scoap.cc0[n]; }

  // An objective (net, value) that moves the test forward, or false when the
  // test is no longer possible from here.
  bool objective(GateId& net, std::uint8_t& val, std::vector<GateId>& frontier) {
    if (g[site] == stuck) return false;  // the fault can no longer be excited
    if (g[site] == kX) {
      net = site;
      val = v_not(stuck);
      return true;
    }
    d_frontier(frontier);
    GateId best = kNoGate;
    for (GateId h : frontier) {
      if (!x_path(h)) continue;
      if (best == kNoGate || scoap.co[h] < scoap.co[best] || (scoap.co[h] == scoap.co[best] && h < best)) best = h;
    }
    if (best == kNoGate) return false;
    const GateType t = nl.gate(best).type;
    const auto fi = nl.fanins(best);
    auto open = [&](std::size_t i) { return g[fi[i]] == kX || fpin(best, i) == kX; };
    switch (t) {
      case GateType::And:
      case GateType::Nand:
      case GateType::Or:
      case GateType::Nor: {
        const std::uint8_t nc = (t == GateType::And || t == GateType::Nand) ? k1 : k0;
        for (std::size_t i = 0; i < fi.size(); ++i)
          if (g[fi[i]] == kX) { net = fi[i]; val = nc; return true; }
        break;
      }
      case GateType::Xor:
      case GateType::Xnor: {
        GateId pick = kNoGate;
        for (std::size_t i = 0; i < fi.size(); ++i)
          if (g[fi[i]] == kX) {
            const GateId c = fi[i];
            if (pick == kNoGate || std::min(scoap.cc0[c], scoap.cc1[c]) < std::min(scoap.cc0[pick], scoap.cc1[pick]))
              pick = c;
          }
        if (pick != kNoGate) {
          net = pick;
          val = scoap.cc0[pick] <= scoap.cc1[pick] ? k0 : k1;
          return true;
        }
        break;
      }
      case GateType::Mux2: {
        if (effect_on_pin(best, 0)) {  // effect on the select: the data inputs must differ
          for (std::size_t i = 1; i <= 2; ++i)
            if (g[fi[i]] == kX) {
              const std::uint8_t other = g[fi[3 - i]];
              net = fi[i];
              val = other == kX ? k0 : v_not(other);
              return true;
            }
        } else {
          const std::uint8_t want = effect_on_pin(best, 1) ? k0 : k1;
          if (g[fi[0]] == kX) { net = fi[0]; val = want; return true; }
        }
        break;
      }
      default: break;
    }
    // The good machine is settled at every side input; an open faulty value
    // remains. Aim at any open input so its X source gets assigned.
    for (std::size_t i = 0; i < fi.size(); ++i)
      if (open(i)) { net = fi[i]; val = k0; return true; }
    return false;
  }

  // Walks back from an objective to an unassigned source (Goel's backtrace).
  std::pair<GateId, std::uint8_t> backtrace(GateId net, std::uint8_t val) {
    while (!is_source(nl.gate(net).type)) {
      const GateType t = nl.gate(net).type;
      const auto fi = nl.fanins(net);
      std::uint8_t want = inverting(t) ? v_not(val) : val;
      GateId next = kNoGate;
      auto open_g = [&](GateId x) { return g[x] == kX; };
      auto open_any = [&](std::size_t i) { return g[fi[i]] == kX || fpin(net, i) == kX; };
      switch (t) {
        case GateType::Buf:
        case GateType::Not: next = fi[0]; break;
        case GateType::And:
        case GateType::Nand:
        case GateType::Or:
        case GateType::Nor: {
          // want is the value needed at the input side of the AND/OR core.
          const bool and_like = t == GateType::And || t == GateType::Nand;
          const bool all_inputs = and_like ? want == k1 : want == k0;  // every input must be set
          for (GateId x : fi) {
            if (!open_g(x)) continue;
            if (next == kNoGate) { next = x; continue; }
            const auto a = cc(x, want), b = cc(next, want);
            if (all_inputs ? a > b : a < b) next = x;
          }
          break;
        }
        case GateType::Xor:
        case GateType::Xnor: {
          std::uint8_t parity = 0;
          for (GateId x : fi) {
            if (open_g(x)) {
              if (next == kNoGate || std::min(scoap.cc0[x], scoap.cc1[x]) < std::min(scoap.cc0[next], scoap.cc1[next]))
                next = x;
            } else {
              parity ^= g[x];
            }
          }
          // Other open inputs are taken as 0 for now.
          want = std::uint8_t(want ^ parity);
          break;
        }
        case GateType::Mux2: {
          const std::uint8_t s = g[fi[0]], a = g[fi[1]], b = g[fi[2]];
          want = val;
          if (s == kX) {
            if (a == val) { next = fi[0]; want = k0; }
            else if (b == val) { next = fi[0]; want = k1; }
            else if (a == kX && b == kX) next = cc(fi[1], val) <= cc(fi[2], val) ? fi[1] : fi[2];
            else if (a == kX) next = fi[1];
            else if (b == kX) next = fi[2];
          } else {
            next = fi[s == k0 ? 1 : 2];
          }
          break;
        }
        default: break;
      }
      if (next == kNoGate || !open_g(next)) {
        // Only the faulty machine is open here; follow any open input.
        next = kNoGate;
        for (std::size_t i = 0; i < fi.size(); ++i)
          if (open_any(i)) { next = fi[i]; break; }
        if (next == kNoGate) throw std::logic_error("PODEM backtrace found no open input at " + nl.gate(net).name);
        want = k0;
      }
      net = next;
      val = want;
    }
    return {net, val};
  }

  PodemResult run(const Fault& flt, const PodemConfig& cfg) {
    PodemResult r;
    trace = cfg.trace ? &r.trace : nullptr;
    setup(flt);
    struct Decision {
      GateId src;
      std::uint8_t val;
      bool flipped;
    };
    std::vector<Decision> stack;
    std::vector<GateId> frontier;
    auto note = [&](const std::string& s) { if (trace) trace->push_back(s); };
    note("fault " + describe(nl, flt));
    while (true) {
      if (detected()) {
        r.outcome = AtpgOutcome::Detected;
        r.cube.assign(sources.size(), -1);
        for (const auto& d : stack) r.cube[source_index[d.src]] = static_cast<std::int8_t>(d.val);
        note("detected");
        return r;
      }
      GateId net = kNoGate;
      std::uint8_t val = kX;
      if (objective(net, val, frontier)) {
        note("objective " + nl.gate(net).name + "=" + vname(val));
        const auto [src, v] = backtrace(net, val);
        note("backtrace -> " + nl.gate(src).name + "=" + vname(v));
        stack.push_back({src, v, false});
        ++r.decisions;
        assign(src, v);
        continue;
      }
      // Backtrack.
      while (!stack.empty() && stack.back().flipped) {
        assign(stack.back().src, kX);
        stack.pop_back();
      }
      if (stack.empty()) {
        r.outcome = AtpgOutcome::Redundant;
        note("search space exhausted: redundant");
        return r;
      }
      if (cfg.backtrack_limit && r.backtracks >= cfg.backtrack_limit) {
        r.outcome = AtpgOutcome::Aborted;
        note("backtrack limit reached: aborted");
        return r;
      }
      auto& d = stack.back();
      d.val = v_not(d.val);
      d.flipped = true;
      ++r.backtracks;
      note("backtrack " + nl.gate(d.src).name + "=" + vname(d.val));
      assign(d.src, d.val);
    }
  }
};

Podem::Podem(const Netlist& nl) : nl_(nl), impl_(std::make_shared<Impl>(nl)) {
  if (!nl.finalized()) throw NetlistError("Podem requires a finalized netlist");
}

PodemResult Podem::generate(const Fault& f, const PodemConfig& cfg) { return impl_->run(f, cfg); }

// ---------------------------------------------------------------------------

namespace {

// Packs up to 64 cubes into simulation words (one per source).
void pack(const std::vector<TestCube>& cubes, std::size_t lo, std::size_t n, std::size_t pis, std::size_t flops,
          std::vector<Word>& pi, std::vector<Word>& st) {
  pi.assign(pis, 0);
  st.assign(flops, 0);
  for (std::size_t k = 0; k < n; ++k) {
    const TestCube& c = cubes[lo + k];
    for (std::size_t i = 0; i < pis; ++i)
      if (c[i] == 1) pi[i] |= Word{1} << k;
    for (std::size_t i = 0; i < flops; ++i)
      if (c[pis + i] == 1) st[i] |= Word{1} << k;
  }
}

Word valid_mask(std::size_t n) { return n >= 64 ? kAllOnes : (Word{1} << n) - 1; }

}  // namespace

AtpgResult run_atpg(const Netlist& nl, const AtpgConfig& cfg) {
  AtpgResult res;
  res.fault_list = build_fault_list(nl, true);
  const FaultList& fl = res.fault_list;
  const std::size_t nf = fl.faults.size();
  res.faults = nf;
  res.uncollapsed = fl.uncollapsed;
  res.status.assign(nf, AtpgOutcome::Aborted);
  const std::size_t pis = nl.inputs().size(), nflops = nl.flops().size(), width = pis + nflops;
  std::mt19937_64 rng(cfg.seed);

  FaultSimulator sim(nl, fl, cfg.threads);
  std::vector<TestCube> patterns;
  std::vector<Word> pi, st;

  // 1. Random patterns, keeping only those that are the first to detect a fault.
  if (cfg.random_patterns) {
    std::size_t idle = 0;
    for (std::size_t done = 0; done < cfg.random_patterns && sim.detected() < nf && idle < cfg.random_patience;) {
      const std::size_t n = std::min<std::size_t>(64, cfg.random_patterns - done);
      std::vector<TestCube> block(n, TestCube(width));
      pi.assign(pis, 0);
      st.assign(nflops, 0);
      for (auto& w : pi) w = rng();
      for (auto& w : st) w = rng();
      for (std::size_t k = 0; k < n; ++k) {
        for (std::size_t i = 0; i < pis; ++i) block[k][i] = static_cast<std::int8_t>(pi[i] >> k & 1);
        for (std::size_t i = 0; i < nflops; ++i) block[k][pis + i] = static_cast<std::int8_t>(st[i] >> k & 1);
      }
      const std::size_t base = sim.patterns_simulated();
      const std::size_t got = sim.simulate_block(pi, st, valid_mask(n));
      idle = got ? 0 : idle + 1;
      std::vector<char> useful(n, 0);
      for (std::size_t i = 0; i < nf; ++i) {
        const auto p = sim.first_detection(i);
        if (p >= static_cast<std::int64_t>(base) && p < static_cast<std::int64_t>(base + n)) useful[p - base] = 1;
      }
      for (std::size_t k = 0; k < n; ++k)
        if (useful[k]) patterns.push_back(block[k]);
      done += n;
    }
  }
  res.random_patterns_kept = patterns.size();

  // 2. PODEM for every fault still undetected. Patterns are fault-simulated in
  // blocks of 64, and each must detect the fault it was generated for.
  Podem podem(nl);
  PodemConfig pc;
  pc.backtrack_limit = cfg.backtrack_limit;
  std::vector<TestCube> pending;
  std::vector<std::size_t> targets;
  auto flush = [&] {
    if (pending.empty()) return;
    pack(pending, 0, pending.size(), pis, nflops, pi, st);
    sim.simulate_block(pi, st, valid_mask(pending.size()));
    for (std::size_t k = 0; k < targets.size(); ++k)
      if (sim.status(targets[k]) != FaultStatus::Detected)
        res.errors.push_back("pattern for " + describe(nl, fl.faults[targets[k]]) + " does not detect it");
    patterns.insert(patterns.end(), pending.begin(), pending.end());
    res.deterministic_patterns += pending.size();
    pending.clear();
    targets.clear();
  };
  for (std::size_t i = 0; i < nf; ++i) {
    if (sim.status(i) == FaultStatus::Detected) continue;
    const auto r = podem.generate(fl.faults[i], pc);
    res.backtracks += r.backtracks;
    if (r.outcome == AtpgOutcome::Redundant) {
      res.status[i] = AtpgOutcome::Redundant;
      continue;
    }
    if (r.outcome == AtpgOutcome::Aborted) continue;
    TestCube c = r.cube;
    for (auto& v : c)
      if (v < 0) v = static_cast<std::int8_t>(rng() & 1);
    pending.push_back(std::move(c));
    targets.push_back(i);
    if (pending.size() == 64) flush();
  }
  flush();
  // Faults that a later pattern detected after PODEM gave up on them.
  for (std::size_t i = 0; i < nf; ++i)
    if (sim.status(i) == FaultStatus::Detected) res.status[i] = AtpgOutcome::Detected;

  // 3. Reverse-order compaction: replay the patterns newest first and keep only
  // those that detect something new.
  if (cfg.compact && !patterns.empty()) {
    FaultSimulator again(nl, fl, cfg.threads);
    std::vector<TestCube> rev(patterns.rbegin(), patterns.rend()), kept;
    for (std::size_t lo = 0; lo < rev.size(); lo += 64) {
      const std::size_t n = std::min<std::size_t>(64, rev.size() - lo);
      pack(rev, lo, n, pis, nflops, pi, st);
      const std::size_t base = again.patterns_simulated();
      again.simulate_block(pi, st, valid_mask(n));
      std::vector<char> useful(n, 0);
      for (std::size_t i = 0; i < nf; ++i) {
        const auto p = again.first_detection(i);
        if (p >= static_cast<std::int64_t>(base) && p < static_cast<std::int64_t>(base + n)) useful[p - base] = 1;
      }
      for (std::size_t k = 0; k < n; ++k)
        if (useful[k]) kept.push_back(rev[lo + k]);
    }
    if (again.detected() != sim.detected())
      res.errors.push_back("compaction changed the number of detected faults");
    patterns.assign(kept.rbegin(), kept.rend());
  }

  res.patterns = std::move(patterns);
  for (std::size_t i = 0; i < nf; ++i) {
    switch (res.status[i]) {
      case AtpgOutcome::Detected: ++res.detected; res.detected_uncollapsed += fl.weight[i]; break;
      case AtpgOutcome::Redundant: ++res.redundant; res.redundant_uncollapsed += fl.weight[i]; break;
      case AtpgOutcome::Aborted: ++res.aborted; break;
    }
  }
  return res;
}

void write_patterns(std::ostream& out, const Netlist& nl, const std::vector<TestCube>& patterns) {
  out << "# rithereal test patterns for " << nl.name << " (full scan, stuck-at)\n";
  out << "# " << patterns.size() << " patterns; columns: inputs, then scan cells\n";
  out << "# inputs:";
  for (GateId i : nl.inputs()) out << " " << nl.gate(i).name;
  out << "\n# scan cells:";
  for (GateId i : nl.flops()) out << " " << nl.gate(i).name;
  out << "\n";
  for (const auto& p : patterns) {
    for (std::size_t i = 0; i < p.size(); ++i) {
      if (i == nl.inputs().size()) out << ' ';
      out << (p[i] < 0 ? 'X' : char('0' + p[i]));
    }
    out << "\n";
  }
}

}  // namespace rith
