// SPDX-License-Identifier: Apache-2.0
#include "rith/sat.hpp"

#include <algorithm>
#include <cmath>

namespace rith {

namespace {
constexpr std::uint32_t kNoPos = 0xFFFFFFFFu;

// Luby sequence 1 1 2 1 1 2 4 ... (restart lengths).
double luby(double y, std::uint64_t x) {
  std::uint64_t size = 1, seq = 0;
  while (size < x + 1) { ++seq; size = 2 * size + 1; }
  while (size - 1 != x) { size = (size - 1) >> 1; --seq; x = x % size; }
  return std::pow(y, static_cast<double>(seq));
}
}  // namespace

std::uint32_t SatSolver::new_var() {
  const auto v = static_cast<std::uint32_t>(assign_.size());
  assign_.push_back(2);
  phase_.push_back(1);  // first try false (phase stores the negation bit)
  model_.push_back(0);
  seen_.push_back(0);
  level_.push_back(0);
  reason_.push_back(kNoReason);
  activity_.push_back(0.0);
  watches_.emplace_back();
  watches_.emplace_back();
  heap_pos_.push_back(kNoPos);
  heap_insert(v);
  return v;
}

void SatSolver::heap_up(std::uint32_t i) {
  const std::uint32_t v = heap_[i];
  while (i > 0) {
    const std::uint32_t p = (i - 1) / 2;
    if (activity_[heap_[p]] >= activity_[v]) break;
    heap_[i] = heap_[p];
    heap_pos_[heap_[i]] = i;
    i = p;
  }
  heap_[i] = v;
  heap_pos_[v] = i;
}

void SatSolver::heap_down(std::uint32_t i) {
  const std::uint32_t v = heap_[i];
  const auto n = static_cast<std::uint32_t>(heap_.size());
  while (true) {
    std::uint32_t c = 2 * i + 1;
    if (c >= n) break;
    if (c + 1 < n && activity_[heap_[c + 1]] > activity_[heap_[c]]) ++c;
    if (activity_[heap_[c]] <= activity_[v]) break;
    heap_[i] = heap_[c];
    heap_pos_[heap_[i]] = i;
    i = c;
  }
  heap_[i] = v;
  heap_pos_[v] = i;
}

void SatSolver::heap_insert(std::uint32_t v) {
  if (heap_pos_[v] != kNoPos) return;
  heap_pos_[v] = static_cast<std::uint32_t>(heap_.size());
  heap_.push_back(v);
  heap_up(heap_pos_[v]);
}

void SatSolver::bump(std::uint32_t v) {
  if ((activity_[v] += var_inc_) > 1e100) {
    for (auto& a : activity_) a *= 1e-100;
    var_inc_ *= 1e-100;
  }
  if (heap_pos_[v] != kNoPos) heap_up(heap_pos_[v]);
}

std::uint32_t SatSolver::attach(const std::vector<SatLit>& lits, bool learnt, std::uint32_t lbd) {
  const auto ci = static_cast<std::uint32_t>(clauses_.size());
  clauses_.push_back({static_cast<std::uint32_t>(lits_.size()), static_cast<std::uint32_t>(lits.size()), lbd, learnt,
                      false, 0.0f});
  lits_.insert(lits_.end(), lits.begin(), lits.end());
  watches_[sat_not(lits[0])].push_back(ci);
  watches_[sat_not(lits[1])].push_back(ci);
  return ci;
}

bool SatSolver::add_clause(std::vector<SatLit> lits) {
  if (!ok_) return false;
  std::sort(lits.begin(), lits.end());
  std::vector<SatLit> c;
  for (std::size_t i = 0; i < lits.size(); ++i) {
    const SatLit l = lits[i];
    if (i && l == lits[i - 1]) continue;
    if (i && l == sat_not(lits[i - 1])) return true;  // tautology
    const auto v = value(l);
    if (v == 1 && level_[sat_var(l)] == 0) return true;  // already satisfied
    if (v == 0 && level_[sat_var(l)] == 0) continue;     // false literal
    c.push_back(l);
  }
  if (c.empty()) return ok_ = false;
  if (c.size() == 1) {
    enqueue(c[0], kNoReason);
    if (propagate() != kNoReason) ok_ = false;
    return ok_;
  }
  attach(c, false, 0);
  return true;
}

void SatSolver::enqueue(SatLit l, std::uint32_t reason) {
  const std::uint32_t v = sat_var(l);
  assign_[v] = static_cast<std::uint8_t>(l & 1);  // assign_ = 0 means the var is true
  level_[v] = static_cast<std::uint32_t>(trail_lim_.size());
  reason_[v] = reason;
  trail_.push_back(l);
}

// Watches: watches_[x] holds clauses with a watched literal whose negation is
// x, so when x becomes true they need a look.
std::uint32_t SatSolver::propagate() {
  while (qhead_ < trail_.size()) {
    const SatLit p = trail_[qhead_++];  // p became true; clauses watching ~p
    auto& ws = watches_[p];
    std::size_t i = 0, j = 0;
    const SatLit false_lit = sat_not(p);
    while (i < ws.size()) {
      const std::uint32_t ci = ws[i++];
      Clause& c = clauses_[ci];
      if (c.deleted) continue;
      SatLit* L = &lits_[c.start];
      if (L[0] == false_lit) std::swap(L[0], L[1]);
      if (value(L[0]) == 1) { ws[j++] = ci; continue; }
      bool found = false;
      for (std::uint32_t k = 2; k < c.size; ++k)
        if (value(L[k]) != 0) {
          std::swap(L[1], L[k]);
          watches_[sat_not(L[1])].push_back(ci);
          found = true;
          break;
        }
      if (found) continue;
      ws[j++] = ci;
      if (value(L[0]) == 0) {  // conflict
        while (i < ws.size()) ws[j++] = ws[i++];
        ws.resize(j);
        qhead_ = trail_.size();
        return ci;
      }
      enqueue(L[0], ci);
    }
    ws.resize(j);
  }
  return kNoReason;
}

bool SatSolver::redundant(SatLit l, std::uint32_t abstract_levels) {
  stack_.clear();
  stack_.push_back(l);
  const std::size_t top = to_clear_.size();
  while (!stack_.empty()) {
    const std::uint32_t r = reason_[sat_var(stack_.back())];
    stack_.pop_back();
    const Clause& c = clauses_[r];
    for (std::uint32_t k = 1; k < c.size; ++k) {
      const SatLit q = lits_[c.start + k];
      const std::uint32_t v = sat_var(q);
      if (seen_[v] || level_[v] == 0) continue;
      if (reason_[v] != kNoReason && (abstract_levels & (1u << (level_[v] & 31)))) {
        seen_[v] = 1;
        stack_.push_back(q);
        to_clear_.push_back(q);
      } else {
        for (std::size_t m = top; m < to_clear_.size(); ++m) seen_[sat_var(to_clear_[m])] = 0;
        to_clear_.resize(top);
        return false;
      }
    }
  }
  return true;
}

void SatSolver::analyze(std::uint32_t confl, std::vector<SatLit>& learnt, std::uint32_t& bt_level,
                        std::uint32_t& lbd) {
  learnt.assign(1, 0);
  std::uint32_t pending = 0;
  SatLit p = 0;
  bool first = true;
  std::size_t index = trail_.size();
  const auto cur = static_cast<std::uint32_t>(trail_lim_.size());
  do {
    Clause& c = clauses_[confl];
    if (c.learnt) {
      if ((c.activity += cla_inc_) > 1e20f) {
        for (auto ci : learnts_) clauses_[ci].activity *= 1e-20f;
        cla_inc_ *= 1e-20f;
      }
    }
    for (std::uint32_t k = first ? 0 : 1; k < c.size; ++k) {
      const SatLit q = lits_[c.start + k];
      const std::uint32_t v = sat_var(q);
      if (seen_[v] || level_[v] == 0) continue;
      seen_[v] = 1;
      bump(v);
      if (level_[v] >= cur) ++pending;
      else learnt.push_back(q);
    }
    first = false;
    while (!seen_[sat_var(trail_[--index])]) {}
    p = trail_[index];
    confl = reason_[sat_var(p)];
    seen_[sat_var(p)] = 0;
    --pending;
    if (pending > 0) {
      // The reason clause has p at position 0 (it was the propagated literal).
      Clause& rc = clauses_[confl];
      SatLit* L = &lits_[rc.start];
      if (L[0] != p) for (std::uint32_t k = 1; k < rc.size; ++k) if (L[k] == p) { std::swap(L[0], L[k]); break; }
    }
  } while (pending > 0);
  learnt[0] = sat_not(p);

  // Minimization: drop literals implied by the others.
  to_clear_.assign(learnt.begin(), learnt.end());
  std::uint32_t abstract_levels = 0;
  for (std::size_t k = 1; k < learnt.size(); ++k) abstract_levels |= 1u << (level_[sat_var(learnt[k])] & 31);
  std::size_t j = 1;
  for (std::size_t k = 1; k < learnt.size(); ++k)
    if (reason_[sat_var(learnt[k])] == kNoReason || !redundant(learnt[k], abstract_levels)) learnt[j++] = learnt[k];
  learnt.resize(j);
  for (SatLit l : to_clear_) seen_[sat_var(l)] = 0;
  to_clear_.clear();

  // Backtrack level: the highest level among the other literals (put it at index 1).
  bt_level = 0;
  if (learnt.size() > 1) {
    std::size_t mi = 1;
    for (std::size_t k = 2; k < learnt.size(); ++k)
      if (level_[sat_var(learnt[k])] > level_[sat_var(learnt[mi])]) mi = k;
    std::swap(learnt[1], learnt[mi]);
    bt_level = level_[sat_var(learnt[1])];
  }
  // LBD: number of distinct decision levels.
  std::vector<std::uint32_t> lv;
  for (SatLit l : learnt) lv.push_back(level_[sat_var(l)]);
  std::sort(lv.begin(), lv.end());
  lbd = static_cast<std::uint32_t>(std::unique(lv.begin(), lv.end()) - lv.begin());
}

void SatSolver::backtrack(std::uint32_t level) {
  if (trail_lim_.size() <= level) return;
  for (std::size_t k = trail_.size(); k-- > trail_lim_[level];) {
    const std::uint32_t v = sat_var(trail_[k]);
    phase_[v] = assign_[v];
    assign_[v] = 2;
    reason_[v] = kNoReason;
    heap_insert(v);
  }
  trail_.resize(trail_lim_[level]);
  trail_lim_.resize(level);
  qhead_ = trail_.size();
}

SatLit SatSolver::pick() {
  while (!heap_.empty()) {
    const std::uint32_t v = heap_[0];
    heap_[0] = heap_.back();
    heap_pos_[heap_[0]] = 0;
    heap_.pop_back();
    heap_pos_[v] = kNoPos;
    if (!heap_.empty()) heap_down(0);
    if (assign_[v] == 2) return 2 * v + phase_[v];
  }
  return 0xFFFFFFFFu;
}

void SatSolver::reduce() {
  // Keep clauses with small LBD and the more active half of the rest.
  std::vector<std::uint32_t> cand;
  for (auto ci : learnts_) {
    const Clause& c = clauses_[ci];
    if (c.deleted) continue;
    const SatLit first = lits_[c.start];
    const bool locked = value(first) == 1 && reason_[sat_var(first)] == ci;
    if (c.lbd > 2 && !locked) cand.push_back(ci);
  }
  std::sort(cand.begin(), cand.end(), [&](std::uint32_t a, std::uint32_t b) {
    return clauses_[a].lbd != clauses_[b].lbd ? clauses_[a].lbd > clauses_[b].lbd
                                              : clauses_[a].activity < clauses_[b].activity;
  });
  for (std::size_t k = 0; k < cand.size() / 2; ++k) clauses_[cand[k]].deleted = true;
  std::vector<std::uint32_t> keep;
  for (auto ci : learnts_)
    if (!clauses_[ci].deleted) keep.push_back(ci);
  learnts_.swap(keep);
}

SatResult SatSolver::solve(std::uint64_t conflict_limit) {
  if (!ok_) return SatResult::Unsat;
  backtrack(0);
  if (propagate() != kNoReason) { ok_ = false; return SatResult::Unsat; }
  std::vector<SatLit> learnt;
  const std::uint64_t start = conflicts_;
  std::uint64_t restarts = 0, next_reduce = 2000;
  while (true) {
    const auto budget = static_cast<std::uint64_t>(100 * luby(2, restarts++));
    std::uint64_t here = 0;
    while (true) {
      const std::uint32_t confl = propagate();
      if (confl != kNoReason) {
        ++conflicts_;
        ++here;
        if (trail_lim_.empty()) { ok_ = false; return SatResult::Unsat; }
        std::uint32_t bt = 0, lbd = 0;
        analyze(confl, learnt, bt, lbd);
        backtrack(bt);
        if (learnt.size() == 1) {
          enqueue(learnt[0], kNoReason);
        } else {
          const std::uint32_t ci = attach(learnt, true, lbd);
          learnts_.push_back(ci);
          enqueue(learnt[0], ci);
        }
        var_inc_ /= 0.95;
        cla_inc_ /= 0.999f;
        if (conflict_limit && conflicts_ - start >= conflict_limit) {
          backtrack(0);
          return SatResult::Unknown;
        }
        continue;
      }
      if (here >= budget) { backtrack(0); break; }  // restart
      if (learnts_.size() >= next_reduce) { reduce(); next_reduce += 1000; }
      const SatLit d = pick();
      if (d == 0xFFFFFFFFu) {  // every variable assigned: a model
        for (std::uint32_t v = 0; v < assign_.size(); ++v) model_[v] = assign_[v] == 0 ? 1 : 0;
        backtrack(0);
        return SatResult::Sat;
      }
      trail_lim_.push_back(static_cast<std::uint32_t>(trail_.size()));
      enqueue(d, kNoReason);
    }
  }
}

}  // namespace rith
