// SPDX-License-Identifier: Apache-2.0
#include "rith/testability.hpp"

#include <algorithm>
#include <limits>

namespace rith {

namespace {
std::uint32_t add(std::uint32_t a, std::uint32_t b) { return std::min<std::uint64_t>(std::uint64_t{a} + b, kScoapInf); }

std::vector<char> observation_points(const Netlist& nl) {
  std::vector<char> obs(nl.size(), 0);
  for (const auto& [name, d] : nl.outputs()) obs[d] = 1;
  for (GateId f : nl.flops()) obs[nl.fanins(f)[0]] = 1;
  return obs;
}
}  // namespace

// ---------------------------------------------------------------------------
// SCOAP
// ---------------------------------------------------------------------------
Scoap compute_scoap(const Netlist& nl) {
  if (!nl.finalized()) throw NetlistError("compute_scoap() requires finalize()");
  const std::size_t n = nl.size();
  Scoap s;
  s.cc0.assign(n, kScoapInf);
  s.cc1.assign(n, kScoapInf);
  s.co.assign(n, kScoapInf);

  // Controllability, forward in level order.
  for (GateId g : nl.topo_order()) {
    const auto fi = nl.fanins(g);
    auto& c0 = s.cc0[g];
    auto& c1 = s.cc1[g];
    switch (nl.gate(g).type) {
      case GateType::Input:
      case GateType::Dff:  // scan makes every flop directly loadable
        c0 = c1 = 1;
        break;
      case GateType::Const0: c0 = 0; break;
      case GateType::Const1: c1 = 0; break;
      case GateType::Buf: c0 = add(s.cc0[fi[0]], 1); c1 = add(s.cc1[fi[0]], 1); break;
      case GateType::Not: c0 = add(s.cc1[fi[0]], 1); c1 = add(s.cc0[fi[0]], 1); break;
      case GateType::And:
      case GateType::Nand:
      case GateType::Or:
      case GateType::Nor: {
        const GateType t = nl.gate(g).type;
        const bool and_like = t == GateType::And || t == GateType::Nand;
        // For AND: output 0 needs any input at 0 (min); output 1 needs all inputs at 1 (sum).
        std::uint32_t any = kScoapInf, all = 0;
        for (GateId f : fi) {
          any = std::min(any, and_like ? s.cc0[f] : s.cc1[f]);
          all = add(all, and_like ? s.cc1[f] : s.cc0[f]);
        }
        const std::uint32_t ctrl = add(any, 1), nonctrl = add(all, 1);
        // AND: cc0 = ctrl, cc1 = nonctrl. OR: cc1 = ctrl, cc0 = nonctrl. Inverting swaps.
        std::uint32_t z0 = and_like ? ctrl : nonctrl, z1 = and_like ? nonctrl : ctrl;
        if (t == GateType::Nand || t == GateType::Nor) std::swap(z0, z1);
        c0 = z0;
        c1 = z1;
        break;
      }
      case GateType::Xor:
      case GateType::Xnor: {
        std::uint32_t e0 = s.cc0[fi[0]], e1 = s.cc1[fi[0]];  // cost of even / odd parity so far
        for (std::size_t i = 1; i < fi.size(); ++i) {
          const std::uint32_t a0 = s.cc0[fi[i]], a1 = s.cc1[fi[i]];
          const std::uint32_t n0 = std::min(add(e0, a0), add(e1, a1));
          const std::uint32_t n1 = std::min(add(e0, a1), add(e1, a0));
          e0 = n0;
          e1 = n1;
        }
        c0 = add(e0, 1);
        c1 = add(e1, 1);
        if (nl.gate(g).type == GateType::Xnor) std::swap(c0, c1);
        break;
      }
      case GateType::Mux2: {
        const GateId sel = fi[0], a = fi[1], b = fi[2];
        c0 = add(std::min(add(s.cc0[sel], s.cc0[a]), add(s.cc1[sel], s.cc0[b])), 1);
        c1 = add(std::min(add(s.cc0[sel], s.cc1[a]), add(s.cc1[sel], s.cc1[b])), 1);
        break;
      }
      case GateType::Undefined: break;
    }
  }

  // Observability, backward. Each fanout branch gives the stem a candidate; the stem takes the min.
  const auto obs = observation_points(nl);
  for (GateId g = 0; g < n; ++g)
    if (obs[g]) s.co[g] = 0;
  const auto& order = nl.topo_order();
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const GateId g = *it;
    const GateType t = nl.gate(g).type;
    if (is_source(t)) continue;  // inputs, constants and flops have no fanins in the scan view
    const std::uint32_t cog = s.co[g];
    if (cog >= kScoapInf) continue;
    const auto fi = nl.fanins(g);
    auto offer = [&](GateId f, std::uint32_t v) { s.co[f] = std::min(s.co[f], v); };
    for (std::size_t p = 0; p < fi.size(); ++p) {
      std::uint32_t side = 0;  // cost of setting the other inputs to sensitize pin p
      switch (t) {
        case GateType::Buf:
        case GateType::Not: break;
        case GateType::And:
        case GateType::Nand:
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side = add(side, s.cc1[fi[q]]);
          break;
        case GateType::Or:
        case GateType::Nor:
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side = add(side, s.cc0[fi[q]]);
          break;
        case GateType::Xor:
        case GateType::Xnor:
          for (std::size_t q = 0; q < fi.size(); ++q)
            if (q != p) side = add(side, std::min(s.cc0[fi[q]], s.cc1[fi[q]]));
          break;
        case GateType::Mux2:
          if (p == 0)  // select is observed when the data inputs differ
            side = std::min(add(s.cc0[fi[1]], s.cc1[fi[2]]), add(s.cc1[fi[1]], s.cc0[fi[2]]));
          else
            side = p == 1 ? s.cc0[fi[0]] : s.cc1[fi[0]];
          break;
        default: break;
      }
      offer(fi[p], add(add(cog, side), 1));
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// COP
// ---------------------------------------------------------------------------
Cop compute_cop(const Netlist& nl) {
  if (!nl.finalized()) throw NetlistError("compute_cop() requires finalize()");
  const std::size_t n = nl.size();
  Cop c;
  c.p1.assign(n, 0.5);
  c.obs.assign(n, 0.0);

  for (GateId g : nl.topo_order()) {
    const auto fi = nl.fanins(g);
    double& p = c.p1[g];
    switch (nl.gate(g).type) {
      case GateType::Input:
      case GateType::Dff: p = 0.5; break;
      case GateType::Const0: p = 0.0; break;
      case GateType::Const1: p = 1.0; break;
      case GateType::Buf: p = c.p1[fi[0]]; break;
      case GateType::Not: p = 1.0 - c.p1[fi[0]]; break;
      case GateType::And:
      case GateType::Nand: {
        double q = 1.0;
        for (GateId f : fi) q *= c.p1[f];
        p = nl.gate(g).type == GateType::And ? q : 1.0 - q;
        break;
      }
      case GateType::Or:
      case GateType::Nor: {
        double q = 1.0;  // probability all inputs are 0
        for (GateId f : fi) q *= 1.0 - c.p1[f];
        p = nl.gate(g).type == GateType::Or ? 1.0 - q : q;
        break;
      }
      case GateType::Xor:
      case GateType::Xnor: {
        double q = c.p1[fi[0]];
        for (std::size_t i = 1; i < fi.size(); ++i) {
          const double r = c.p1[fi[i]];
          q = q * (1.0 - r) + (1.0 - q) * r;
        }
        p = nl.gate(g).type == GateType::Xor ? q : 1.0 - q;
        break;
      }
      case GateType::Mux2: {
        const double s = c.p1[fi[0]];
        p = (1.0 - s) * c.p1[fi[1]] + s * c.p1[fi[2]];
        break;
      }
      case GateType::Undefined: break;
    }
  }

  // Observability: a stem is observed if any branch is; branches are treated as
  // independent: obs(stem) = 1 - prod(1 - obs(branch)).
  const auto obs_pts = observation_points(nl);
  std::vector<double> not_obs(n, 1.0);  // running product of (1 - branch observability)
  for (GateId g = 0; g < n; ++g)
    if (obs_pts[g]) not_obs[g] = 0.0;
  const auto& order = nl.topo_order();
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const GateId g = *it;
    c.obs[g] = 1.0 - not_obs[g];
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
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side *= c.p1[fi[q]];
          break;
        case GateType::Or:
        case GateType::Nor:
          for (std::size_t q = 0; q < fi.size(); ++q) if (q != p) side *= 1.0 - c.p1[fi[q]];
          break;
        case GateType::Mux2:
          if (p == 0) {
            const double a = c.p1[fi[1]], b = c.p1[fi[2]];
            side = a * (1.0 - b) + (1.0 - a) * b;
          } else {
            side = p == 1 ? 1.0 - c.p1[fi[0]] : c.p1[fi[0]];
          }
          break;
        default: break;  // BUF, NOT, XOR, XNOR pass changes through
      }
      not_obs[fi[p]] *= 1.0 - og * side;
    }
  }
  return c;
}

}  // namespace rith
