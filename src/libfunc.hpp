// SPDX-License-Identifier: Apache-2.0
// Internal: evaluating Liberty cell functions by truth table.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "rith/liberty.hpp"

namespace rith::libfunc {

// ---------------------------------------------------------------------------
// Boolean functions of Liberty cells, evaluated by truth table
// ---------------------------------------------------------------------------
inline constexpr const char* kState = "@IQ";  // the flop's state, shared name across cells

using Env = std::map<std::string, bool>;

inline bool eval(const LibExpr& e, const LibFlop* ff, const Env& env) {
  switch (e.op) {
    case LibExpr::Op::Const0: return false;
    case LibExpr::Op::Const1: return true;
    case LibExpr::Op::Var: {
      if (ff && e.var == ff->iq) return env.at(kState);
      if (ff && !ff->iqn.empty() && e.var == ff->iqn) return !env.at(kState);
      auto it = env.find(e.var);
      if (it == env.end()) throw std::runtime_error("unknown variable '" + e.var + "'");
      return it->second;
    }
    case LibExpr::Op::Not: return !eval(e.args[0], ff, env);
    case LibExpr::Op::And:
      for (const auto& a : e.args)
        if (!eval(a, ff, env)) return false;
      return true;
    case LibExpr::Op::Or:
      for (const auto& a : e.args)
        if (eval(a, ff, env)) return true;
      return false;
    case LibExpr::Op::Xor: {
      bool v = false;
      for (const auto& a : e.args) v ^= eval(a, ff, env);
      return v;
    }
  }
  return false;
}

// Calls f for every assignment of vars (fixed entries of base are kept).
template <class F>
bool for_all(const std::vector<std::string>& vars, Env base, F&& f) {
  const std::size_t n = vars.size();
  if (n > 16) return false;  // too many inputs to enumerate; treat as "no match"
  for (std::uint32_t m = 0; m < (1u << n); ++m) {
    for (std::size_t k = 0; k < n; ++k) base[vars[k]] = (m >> k) & 1u;
    if (!f(base)) return false;
  }
  return true;
}

inline std::vector<std::string> input_pins(const LibCell& c) {
  std::vector<std::string> v;
  for (const auto& p : c.pins)
    if (p.dir == LibPin::Dir::Input) v.push_back(p.name);
  return v;
}

inline std::optional<LibExpr> parse_opt(const std::string& s) {
  if (s.empty()) return std::nullopt;
  try { return parse_liberty_function(s); }
  catch (const std::exception&) { return std::nullopt; }
}

inline std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}


// The single-output, non-sequential cell with `n_inputs` inputs whose function
// matches `truth` (bit m = output for input assignment m, inputs in pin order).
// Prefers the smallest drive by a simple name rule (X0/X1/_1 first), then the name.
inline const LibCell* find_logic_cell(const Library& lib, std::size_t n_inputs, std::uint32_t truth,
                                      std::vector<std::string>* pin_order = nullptr, std::string* out_pin = nullptr) {
  std::vector<std::pair<std::string, const LibCell*>> found;
  for (const auto& [name, c] : lib.cells) {
    if (c.flop || c.has_latch || c.has_statetable) continue;
    const auto ins = input_pins(c);
    std::vector<const LibPin*> outs;
    for (const auto& p : c.pins)
      if (p.dir == LibPin::Dir::Output) outs.push_back(&p);
    if (ins.size() != n_inputs || outs.size() != 1) continue;
    const auto e = parse_opt(outs[0]->function);
    if (!e) continue;
    bool ok = true;
    try {
      for (std::uint32_t m = 0; m < (1u << n_inputs) && ok; ++m) {
        Env env;
        for (std::size_t k = 0; k < n_inputs; ++k) env[ins[k]] = (m >> k) & 1u;
        ok = eval(*e, nullptr, env) == bool((truth >> m) & 1u);
      }
    } catch (const std::exception&) {
      ok = false;
    }
    if (ok) found.emplace_back(name, &c);
  }
  if (found.empty()) return nullptr;
  auto rank = [](const std::string& n) {
    const std::string u = upper(n);
    if (u.find("X0") != std::string::npos) return 0;
    if (u.find("X1") != std::string::npos || u.find("_1") != std::string::npos) return 1;
    return 2;
  };
  std::sort(found.begin(), found.end(), [&](const auto& a, const auto& b) {
    if (rank(a.first) != rank(b.first)) return rank(a.first) < rank(b.first);
    if (a.first.size() != b.first.size()) return a.first.size() < b.first.size();
    return a.first < b.first;
  });
  const LibCell* c = found[0].second;
  if (pin_order) *pin_order = input_pins(*c);
  if (out_pin)
    for (const auto& p : c->pins)
      if (p.dir == LibPin::Dir::Output) *out_pin = p.name;
  return c;
}

}  // namespace rith::libfunc
