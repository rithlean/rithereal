// SPDX-License-Identifier: Apache-2.0
// A small CDCL SAT solver, written for ATPG: two watched literals, first-UIP
// clause learning with minimization, VSIDS decisions with phase saving, Luby
// restarts and learned-clause cleanup by LBD. It follows the design of MiniSat
// (Een and Sorensson, 2003) and Glucose (Audemard and Simon, 2009) in a much
// smaller form.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rith {

// A literal is 2 * var + (1 if negated).
using SatLit = std::uint32_t;
inline SatLit sat_pos(std::uint32_t v) { return 2 * v; }
inline SatLit sat_neg(std::uint32_t v) { return 2 * v + 1; }
inline SatLit sat_not(SatLit l) { return l ^ 1; }
inline std::uint32_t sat_var(SatLit l) { return l >> 1; }

enum class SatResult : std::uint8_t { Sat, Unsat, Unknown };

class SatSolver {
 public:
  std::uint32_t new_var();
  std::uint32_t num_vars() const { return static_cast<std::uint32_t>(assign_.size()); }
  // Adds a clause (at decision level 0). Returns false once the formula is
  // known to be unsatisfiable.
  bool add_clause(std::vector<SatLit> lits);
  // conflict_limit 0 = no limit; Unknown when the limit is reached.
  SatResult solve(std::uint64_t conflict_limit = 0);
  // After Sat: the value of a variable in the model.
  bool model_value(std::uint32_t v) const { return model_[v] == 1; }
  std::uint64_t conflicts() const { return conflicts_; }

 private:
  struct Clause {
    std::uint32_t start, size;
    std::uint32_t lbd;
    bool learnt, deleted;
    float activity;
  };
  static constexpr std::uint32_t kNoReason = 0xFFFFFFFFu;
  // value of a literal: 1 true, 0 false, 2 unassigned
  std::uint8_t value(SatLit l) const {
    const std::uint8_t a = assign_[sat_var(l)];  // 0 true, 1 false, 2 unassigned
    return a == 2 ? 2 : static_cast<std::uint8_t>(1 ^ a ^ (l & 1));
  }
  void enqueue(SatLit l, std::uint32_t reason);
  std::uint32_t propagate();  // conflicting clause or kNoReason
  void analyze(std::uint32_t confl, std::vector<SatLit>& learnt, std::uint32_t& bt_level, std::uint32_t& lbd);
  bool redundant(SatLit l, std::uint32_t abstract_levels);
  void backtrack(std::uint32_t level);
  SatLit pick();
  void bump(std::uint32_t v);
  void heap_up(std::uint32_t i);
  void heap_down(std::uint32_t i);
  void heap_insert(std::uint32_t v);
  std::uint32_t attach(const std::vector<SatLit>& lits, bool learnt, std::uint32_t lbd);
  void reduce();

  std::vector<SatLit> lits_;  // clause literals, arena
  std::vector<Clause> clauses_;
  std::vector<std::vector<std::uint32_t>> watches_;  // per literal: clauses watching it
  std::vector<std::uint8_t> assign_, phase_, model_, seen_;
  std::vector<std::uint32_t> level_, reason_;
  std::vector<SatLit> trail_;
  std::vector<std::uint32_t> trail_lim_;
  std::size_t qhead_ = 0;
  std::vector<double> activity_;
  double var_inc_ = 1.0;
  float cla_inc_ = 1.0f;
  std::vector<std::uint32_t> heap_, heap_pos_;  // max-heap on activity
  std::vector<std::uint32_t> learnts_;
  std::uint64_t conflicts_ = 0;
  bool ok_ = true;
  std::vector<SatLit> stack_, to_clear_;
};

}  // namespace rith
