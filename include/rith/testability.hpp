// SPDX-License-Identifier: Apache-2.0
// Testability measures in the full-scan view:
//   SCOAP (Goldstein 1979): integer controllability CC0/CC1 and observability CO,
//     where a larger number means harder.
//   COP (Brglez 1984): probability a net is 1 under random patterns, and the
//     probability a change on it reaches an output.
// These are the features and the baseline for test point insertion.
#pragma once

#include <cstdint>
#include <vector>

#include "rith/netlist.hpp"

namespace rith {

inline constexpr std::uint32_t kScoapInf = 1u << 30;  // uncontrollable or unobservable

struct Scoap {
  std::vector<std::uint32_t> cc0, cc1;  // per gate output (net)
  std::vector<std::uint32_t> co;        // per net (stem)
};

struct Cop {
  std::vector<double> p1;  // probability the net is 1
  std::vector<double> obs; // probability a value change on the net is observed
  // Random-pattern detection probability of a stem fault.
  double detect_sa0(GateId g) const { return p1[g] * obs[g]; }
  double detect_sa1(GateId g) const { return (1.0 - p1[g]) * obs[g]; }
};

Scoap compute_scoap(const Netlist& nl);
Cop compute_cop(const Netlist& nl);

}  // namespace rith
