// SPDX-License-Identifier: Apache-2.0
#include "rith/fault.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <functional>
#include <thread>
#include <numeric>
#include <random>

#include "rith/sim.hpp"

namespace rith {

namespace {

// Places a net is observed or consumed: gate fanouts plus primary outputs.
std::vector<std::uint32_t> branch_counts(const Netlist& nl) {
  std::vector<std::uint32_t> n(nl.size(), 0);
  for (GateId g = 0; g < nl.size(); ++g) n[g] = static_cast<std::uint32_t>(nl.fanouts(g).size());
  for (const auto& [name, d] : nl.outputs()) ++n[d];
  return n;
}

bool is_constant(GateType t) { return t == GateType::Const0 || t == GateType::Const1; }

struct UnionFind {
  std::vector<std::uint32_t> parent;
  explicit UnionFind(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
  std::uint32_t find(std::uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  }
  void unite(std::uint32_t a, std::uint32_t b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);  // keep the earliest as representative
  }
};

}  // namespace

FaultList build_fault_list(const Netlist& nl, bool collapse) {
  if (!nl.finalized()) throw NetlistError("build_fault_list() requires finalize()");
  const auto branches = branch_counts(nl);

  // Enumerate every fault, remembering where each stem and pin fault lives.
  std::vector<Fault> all;
  std::vector<std::int64_t> stem_index(nl.size(), -1);  // index of SA0; SA1 is +1
  std::vector<std::vector<std::int64_t>> pin_index(nl.size());
  // A gate is live if its output can reach an output or a flop's D pin. Faults in
  // dead logic (and on nets that drive nothing, such as clock and reset ports in
  // the scan view) can never be observed, so they are counted as unused.
  std::vector<char> live(nl.size(), 0);
  {
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
  }
  auto pin_live = [&](GateId g) { return live[g] || nl.gate(g).type == GateType::Dff; };

  std::size_t unused = 0;
  for (GateId g = 0; g < nl.size(); ++g) {
    if (is_constant(nl.gate(g).type)) continue;
    if (!live[g]) {
      unused += 2;
      continue;
    }
    stem_index[g] = static_cast<std::int64_t>(all.size());
    all.push_back({g, Fault::kOutputPin, false});
    all.push_back({g, Fault::kOutputPin, true});
  }
  for (GateId g = 0; g < nl.size(); ++g) {
    const auto fi = nl.fanins(g);
    pin_index[g].assign(fi.size(), -1);
    for (std::size_t p = 0; p < fi.size(); ++p) {
      if (branches[fi[p]] < 2 || is_constant(nl.gate(fi[p]).type)) continue;
      if (!pin_live(g)) {
        unused += 2;
        continue;
      }
      pin_index[g][p] = static_cast<std::int64_t>(all.size());
      all.push_back({g, static_cast<std::int32_t>(p), false});
      all.push_back({g, static_cast<std::int32_t>(p), true});
    }
  }

  FaultList out;
  out.uncollapsed = all.size();
  out.unused = unused;
  UnionFind uf(all.size());

  if (collapse) {
    // Index of the fault "input pin p of g stuck at v": the branch fault if the
    // driver fans out, else the driver's own stem fault.
    auto in_fault = [&](GateId g, std::size_t p, bool v) -> std::int64_t {
      const std::int64_t b = pin_index[g][p];
      if (b >= 0) return b + v;
      const GateId d = nl.fanins(g)[p];
      return stem_index[d] < 0 ? -1 : stem_index[d] + v;
    };
    auto join = [&](std::int64_t a, std::int64_t b) {
      if (a >= 0 && b >= 0) uf.unite(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    };
    for (GateId g = 0; g < nl.size(); ++g) {
      const GateType t = nl.gate(g).type;
      const std::int64_t out0 = stem_index[g];
      if (out0 < 0) continue;
      const std::size_t n = nl.fanins(g).size();
      for (std::size_t p = 0; p < n; ++p) {
        switch (t) {
          case GateType::And: join(in_fault(g, p, false), out0 + 0); break;   // in SA0 == out SA0
          case GateType::Nand: join(in_fault(g, p, false), out0 + 1); break;  // in SA0 == out SA1
          case GateType::Or: join(in_fault(g, p, true), out0 + 1); break;     // in SA1 == out SA1
          case GateType::Nor: join(in_fault(g, p, true), out0 + 0); break;    // in SA1 == out SA0
          case GateType::Buf:
            join(in_fault(g, p, false), out0 + 0);
            join(in_fault(g, p, true), out0 + 1);
            break;
          case GateType::Not:
            join(in_fault(g, p, false), out0 + 1);
            join(in_fault(g, p, true), out0 + 0);
            break;
          default: break;  // XOR, XNOR, MUX, DFF: no structural equivalences
        }
      }
    }
  }

  std::vector<std::int64_t> rep_slot(all.size(), -1);
  for (std::size_t i = 0; i < all.size(); ++i) {
    const auto r = uf.find(static_cast<std::uint32_t>(i));
    if (rep_slot[r] < 0) {
      rep_slot[r] = static_cast<std::int64_t>(out.faults.size());
      out.faults.push_back(all[r]);
      out.weight.push_back(0);
    }
    ++out.weight[static_cast<std::size_t>(rep_slot[r])];
  }
  return out;
}

std::string describe(const Netlist& nl, const Fault& f) {
  const std::string sa = f.stuck_at_one ? " SA1" : " SA0";
  if (f.pin == Fault::kOutputPin) return nl.gate(f.gate).name + sa;
  const GateId d = nl.fanins(f.gate)[static_cast<std::size_t>(f.pin)];
  return nl.gate(d).name + " -> " + nl.gate(f.gate).name + ".in" + std::to_string(f.pin) + sa;
}

// ---------------------------------------------------------------------------
// Fault simulator
// ---------------------------------------------------------------------------
FaultSimulator::FaultSimulator(const Netlist& nl, FaultList faults, unsigned threads)
    : nl_(nl), faults_(std::move(faults)) {
  if (!nl.finalized()) throw NetlistError("FaultSimulator requires a finalized netlist");
  status_.assign(faults_.faults.size(), FaultStatus::Undetected);
  first_pattern_.assign(faults_.faults.size(), -1);
  for (auto w : faults_.weight) total_weight_ += w;
  observed_.assign(nl.size(), 0);
  for (const auto& [name, d] : nl.outputs()) observed_[d] = 1;
  for (GateId f : nl.flops()) observed_[nl.fanins(f)[0]] = 1;  // pseudo-outputs
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  workers_.resize(threads);
  for (auto& w : workers_) {
    w.queued.assign(nl.size(), 0);
    w.buckets.resize(nl.depth() + 1);
  }
}

FaultSimulator::~FaultSimulator() = default;

double FaultSimulator::coverage() const {
  return faults_.faults.empty() ? 1.0 : static_cast<double>(detected_) / static_cast<double>(faults_.faults.size());
}
double FaultSimulator::coverage_uncollapsed() const {
  return total_weight_ == 0 ? 1.0 : static_cast<double>(detected_weight_) / static_cast<double>(total_weight_);
}

void FaultSimulator::schedule_fanouts(Worker& w, GateId g) const {
  for (GateId h : nl_.fanouts(g)) {
    if (w.queued[h] || is_source(nl_.gate(h).type)) continue;  // flops are observation points, not gates
    w.queued[h] = 1;
    w.buckets[nl_.level(h)].push_back(h);
    ++w.pending;
  }
}

// Injects `start_value` on gate `start` (which differs from the good value in at
// least one bit) and propagates it event-driven, level by level. Returns the
// mask of patterns where the difference reaches an observation point.
Word FaultSimulator::propagate(Worker& w, GateId start, Word start_value) const {
  Word detected = 0;
  auto& faulty = w.faulty;
  faulty[start] = start_value;
  w.touched.push_back(start);
  if (observed_[start]) detected |= start_value ^ good_[start];
  schedule_fanouts(w, start);

  for (std::size_t lvl = nl_.level(start) + 1; w.pending && lvl < w.buckets.size(); ++lvl) {
    auto& bucket = w.buckets[lvl];
    for (std::size_t k = 0; k < bucket.size(); ++k) {
      const GateId h = bucket[k];
      w.queued[h] = 0;
      --w.pending;
      const auto fi = nl_.fanins(h);
      const Word v = eval_gate(nl_.gate(h).type, fi.size(), [&](std::size_t i) { return faulty[fi[i]]; });
      if (v == good_[h]) continue;
      faulty[h] = v;
      w.touched.push_back(h);
      if (observed_[h]) detected |= v ^ good_[h];
      schedule_fanouts(w, h);
    }
    bucket.clear();
  }
  for (GateId g : w.touched) faulty[g] = good_[g];
  w.touched.clear();
  return detected;
}

Word FaultSimulator::detect_mask(Worker& w, const Fault& f) const {
  const Word stuck = f.stuck_at_one ? kAllOnes : 0;
  if (f.pin == Fault::kOutputPin) {
    if (good_[f.gate] == stuck) return 0;  // never activated
    return propagate(w, f.gate, stuck);
  }
  const GateId g = f.gate;
  const auto fi = nl_.fanins(g);
  const auto pin = static_cast<std::size_t>(f.pin);
  if (nl_.gate(g).type == GateType::Dff)  // the flop's D pin is itself observed
    return good_[fi[pin]] ^ stuck;
  const Word v = eval_gate(nl_.gate(g).type, fi.size(),
                           [&](std::size_t i) { return i == pin ? stuck : good_[fi[i]]; });
  if (v == good_[g]) return 0;
  return propagate(w, g, v);
}

Word FaultSimulator::detect_mask(const Fault& f) { return detect_mask(workers_.front(), f); }

std::size_t FaultSimulator::simulate_block(std::span<const Word> pi, std::span<const Word> state, Word valid) {
  good_ = simulate(nl_, pi, state);
  for (auto& w : workers_) w.faulty = good_;

  // Hand out undetected faults in small chunks so threads stay evenly loaded.
  std::vector<std::uint32_t> todo;
  todo.reserve(faults_.faults.size() - detected_);
  for (std::size_t i = 0; i < faults_.faults.size(); ++i)
    if (status_[i] == FaultStatus::Undetected) todo.push_back(static_cast<std::uint32_t>(i));
  std::vector<Word> masks(todo.size(), 0);
  std::atomic<std::size_t> next{0};
  constexpr std::size_t kChunk = 256;
  auto work = [&](Worker& w) {
    for (std::size_t lo; (lo = next.fetch_add(kChunk)) < todo.size();)
      for (std::size_t k = lo; k < std::min(lo + kChunk, todo.size()); ++k)
        masks[k] = detect_mask(w, faults_.faults[todo[k]]) & valid;
  };
  const std::size_t nthreads = std::min<std::size_t>(workers_.size(), todo.size() / kChunk + 1);
  if (nthreads <= 1) {
    work(workers_.front());
  } else {
    std::vector<std::thread> pool;
    for (std::size_t t = 1; t < nthreads; ++t) pool.emplace_back(work, std::ref(workers_[t]));
    work(workers_.front());
    for (auto& th : pool) th.join();
  }

  std::size_t newly = 0;
  for (std::size_t k = 0; k < todo.size(); ++k) {
    if (!masks[k]) continue;
    const std::size_t i = todo[k];
    status_[i] = FaultStatus::Detected;
    first_pattern_[i] = static_cast<std::int64_t>(patterns_) + std::countr_zero(masks[k]);
    ++detected_;
    detected_weight_ += faults_.weight[i];
    ++newly;
  }
  patterns_ += static_cast<std::size_t>(std::popcount(valid));
  return newly;
}

RandomFaultSimResult run_random_patterns(FaultSimulator& sim, std::size_t max_patterns, std::uint64_t seed,
                                         std::size_t patience, std::span<const std::pair<std::size_t, bool>> fixed) {
  const Netlist& nl = sim.netlist();
  std::mt19937_64 rng(seed);
  std::vector<Word> pi(nl.inputs().size()), st(nl.flops().size());
  std::size_t quiet = 0;
  const std::size_t total = sim.fault_list().faults.size();
  while (sim.patterns_simulated() < max_patterns && sim.detected() < total && (patience == 0 || quiet < patience)) {
    for (auto& w : pi) w = rng();
    for (auto& w : st) w = rng();
    for (const auto& [i, v] : fixed) pi.at(i) = v ? kAllOnes : 0;
    const std::size_t left = max_patterns - sim.patterns_simulated();
    const Word valid = left >= 64 ? kAllOnes : ((Word{1} << left) - 1);
    quiet = sim.simulate_block(pi, st, valid) ? 0 : quiet + 1;
  }
  RandomFaultSimResult r;
  r.patterns = sim.patterns_simulated();
  r.detected = sim.detected();
  r.faults = total;
  r.coverage = sim.coverage();
  r.coverage_uncollapsed = sim.coverage_uncollapsed();
  return r;
}

}  // namespace rith
