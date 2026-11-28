// SPDX-License-Identifier: Apache-2.0
// Finds the scan chains already in a netlist, such as Design Compiler output.
//
// The chains are traced by simulating shift mode (scan enable on, every other
// input random) and matching each flop's next state to the one source it
// copies: a scan-in input or another flop, possibly inverted. Walking these
// links back from each scan-out port gives the chains in shift order. This
// sees through buffers, inverters, scan muxes, lockup latches and plain flops
// that shift through their D pin (shift-register segments), and needs no
// knowledge of pin names.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rith/netlist.hpp"

namespace rith {

struct TracedCell {
  GateId flop = kNoGate;
  bool inverted = false;  // holds the scan-in data inverted
};

struct TracedChain {
  std::string si_port, so_port;
  std::vector<TracedCell> cells;  // in shift order, scan-in first
  bool so_inverted = false;       // scan-out sees the scan-in data inverted
};

struct TraceConfig {
  std::string se_port;          // scan enable input
  bool se_active_high = true;
  std::map<std::string, bool> tie;  // other inputs held constant during shift
  std::size_t blocks = 4;       // 64-pattern blocks per signature
  std::uint64_t seed = 1;
};

struct TraceResult {
  std::vector<TracedChain> chains;
  std::vector<GateId> off_chain;  // flops on no traced chain
  std::vector<std::string> broken;  // scan-out ports whose chain does not reach an input
};

TraceResult trace_scan_chains(const Netlist& nl, const TraceConfig& cfg);

}  // namespace rith
