// SPDX-License-Identifier: Apache-2.0
// Test point insertion (TPI) for random-pattern testability.
//
// Test points, in the full-scan view:
//   Observe   a new flop captures the net, so faults behind it become visible.
//   Control0  the net's consumers see  net AND NOT(tp_en AND ctl): forced to 0
//   Control1  the net's consumers see  net OR (tp_en AND ctl):     forced to 1
//             where ctl is a new (scan) flop and tp_en a new input, 0 in mission mode.
// Primary outputs keep observing the original net, so mission mode is unchanged
// with tp_en = 0.
//
// The baseline selector is COP-driven and greedy: each round it shortlists
// candidates by cheap local scores, re-scores each shortlisted test point exactly
// with a full COP pass that includes it, and keeps the best. The cost is the
// expected number of stem faults still undetected after N random patterns,
// sum over faults of (1 - P(detect))^N, taken over live logic only. Every few
// points a fault simulation re-aims the cost at the nets whose faults really
// escape, since COP misjudges reconvergent fanout.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rith/design.hpp"
#include "rith/liberty.hpp"
#include "rith/netlist.hpp"

namespace rith {

enum class TpKind : std::uint8_t { Observe, Control0, Control1 };
std::string_view to_string(TpKind k);

struct TestPoint {
  GateId net = kNoGate;
  TpKind kind = TpKind::Observe;
  bool operator==(const TestPoint&) const = default;
};

// COP of a netlist with test points applied virtually (tp_en = 1, each control
// flop random). Values are for the original nets: p1 before any control point,
// obs of that original net.
struct TpCop {
  std::vector<double> p1, obs;
};
TpCop compute_cop_with(const Netlist& nl, std::span<const TestPoint> tps);

// Expected number of undetected stem faults after `patterns` random patterns.
double tpi_cost(const Netlist& nl, const TpCop& cop, std::size_t patterns);

struct TpiConfig {
  std::size_t max_points = 16;
  std::size_t patterns = 32768;  // the random-pattern budget the cost targets
  std::size_t shortlist = 64;    // candidates re-scored exactly per round
  double min_gain = 0.05;        // stop when the best point removes fewer expected escapes
  bool observe = true, control = true;
  std::vector<char> allowed;     // optional mask over gates; empty means all
  unsigned threads = 0;          // 0 = all hardware threads
  // Fault-simulation guidance: every `refine_every` points, simulate `patterns`
  // random patterns and score only the faults still undetected. COP alone
  // misjudges reconvergent logic; this keeps the picks on real escapes.
  // 0 = pure COP.
  std::size_t refine_every = 4;
  std::uint64_t seed = 1;
};

struct TpiResult {
  std::vector<TestPoint> points;
  std::vector<double> cost;  // cost[0] before, cost[k] after k points
};

// Greedy selection (the classic baseline). Deterministic for a given config.
TpiResult select_test_points(const Netlist& nl, const TpiConfig& cfg = {});

// A copy of the netlist with test points as real gates: input `enable`, flops
// rith_tp_obs<k> / rith_tp_ctl<k>, and the control gates.
Netlist apply_test_points(const Netlist& nl, std::span<const TestPoint> tps, const std::string& enable = "tp_en");

// Random-pattern stuck-at coverage over the faults of the original logic
// (uncollapsed; faults inside test point logic and in unobservable logic left
// out), with `enable` held at 1.
struct TpCoverage {
  std::size_t faults = 0, detected = 0, patterns = 0;
  std::size_t unused = 0;  // faults in logic nothing observes (not counted)
  std::vector<GateId> undetected_nets;  // original nets with an undetected stem or branch fault
  double coverage() const { return faults ? double(detected) / double(faults) : 1.0; }
};
TpCoverage random_coverage(const Netlist& original, std::span<const TestPoint> tps, std::size_t patterns,
                           std::uint64_t seed = 1, const std::string& enable = "tp_en");

// ---- inserting test points into a mapped netlist ----
struct TpDesignConfig {
  std::string enable = "tp_en";
  std::string clock;  // clock port for the new flops; empty = the port clocking the most flops
};
struct TpDesignResult {
  std::size_t flops = 0, gates = 0;
  std::string clock;
  std::vector<std::string> warnings;
};
// Adds the test points (named by net) with library cells: a plain D flop per
// point, AND2/NAND2/OR2 cells for control points. Run scan insertion afterwards
// to put the new flops on chains.
TpDesignResult insert_test_points(Design& d, const Library& lib,
                                  const std::vector<std::pair<std::string, TpKind>>& points,
                                  const TpDesignConfig& cfg = {});

}  // namespace rith
