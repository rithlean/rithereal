// SPDX-License-Identifier: Apache-2.0
// Deterministic test generation for single stuck-at faults, full-scan view.
//
// The engine is PODEM (Goel, "An implicit enumeration algorithm to generate
// tests for combinational logic circuits", IEEE Trans. Computers, 1981):
//
//   loop
//     if the fault effect reaches an output or flop input: success
//     if the test is no longer possible (fault site at the stuck value, empty
//         D-frontier, or no X-path from it): backtrack
//     else: objective -> backtrace to an unassigned input -> assign, imply
//   backtrack: flip the newest decision not yet flipped, dropping later ones;
//     when none is left the fault is redundant.
//
// Decisions are made only on primary inputs and scan flops. Implication is
// three-valued simulation of the good and the faulty machine together, so a
// value pair (1, 0) is D and (0, 1) is D'. Backtrace is guided by SCOAP, as in
// Goel's paper: when every input must be set, go to the hardest one first;
// when any one input will do, go to the easiest.
//
// Choices the paper leaves open, and how they are made here:
//   - D-frontier gate: the one with an X-path whose output has the lowest SCOAP
//     observability (CO), ties broken by gate id.
//   - XOR/XNOR backtrace: the easiest-to-control unassigned input.
//   - A second value is tried for a decision exactly once (complete search);
//     `backtrack_limit` bounds the number of flips, after which the fault is
//     reported as aborted rather than redundant.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rith/fault.hpp"
#include "rith/netlist.hpp"
#include "rith/testability.hpp"

namespace rith {

// One test: a value per primary input (nl.inputs() order) then per flop
// (nl.flops() order, the scan-in state): 0, 1, or -1 for don't care.
using TestCube = std::vector<std::int8_t>;

enum class AtpgOutcome : std::uint8_t { Detected, Redundant, Aborted };
std::string_view to_string(AtpgOutcome o);

struct PodemConfig {
  std::size_t backtrack_limit = 1000;  // 0 = no limit (a complete search)
  bool trace = false;                  // record each step in PodemResult::trace
};

struct PodemResult {
  AtpgOutcome outcome = AtpgOutcome::Aborted;
  TestCube cube;  // valid when Detected
  std::size_t decisions = 0, backtracks = 0;
  std::vector<std::string> trace;
};

class Podem {
 public:
  explicit Podem(const Netlist& nl);
  PodemResult generate(const Fault& f, const PodemConfig& cfg = {});
  const Netlist& netlist() const { return nl_; }

 private:
  struct Impl;
  const Netlist& nl_;
  std::shared_ptr<Impl> impl_;
};

// ---- the whole flow ----
struct AtpgConfig {
  std::size_t random_patterns = 4096;  // random phase first; 0 skips it
  std::size_t random_patience = 8;     // stop the random phase after this many useless blocks of 64
  std::size_t backtrack_limit = 1000;
  bool compact = true;                 // reverse-order fault simulation drops useless patterns
  std::uint64_t seed = 1;              // random phase and X fill
  unsigned threads = 0;                // fault simulation threads
};

struct AtpgResult {
  std::vector<TestCube> patterns;  // fully specified (X filled), in application order
  std::size_t faults = 0;          // collapsed
  std::size_t detected = 0, redundant = 0, aborted = 0;
  std::size_t uncollapsed = 0, detected_uncollapsed = 0, redundant_uncollapsed = 0;
  std::size_t random_patterns_kept = 0;  // of `patterns`, before compaction
  std::size_t deterministic_patterns = 0;
  std::size_t backtracks = 0;
  std::vector<AtpgOutcome> status;   // per collapsed fault, after the run
  std::vector<std::string> errors;   // internal inconsistencies (should stay empty)
  FaultList fault_list;
  double coverage() const { return uncollapsed ? double(detected_uncollapsed) / double(uncollapsed) : 1.0; }
  // Detected or proved redundant, over all faults.
  double efficiency() const {
    return uncollapsed ? double(detected_uncollapsed + redundant_uncollapsed) / double(uncollapsed) : 1.0;
  }
};

AtpgResult run_atpg(const Netlist& nl, const AtpgConfig& cfg = {});

// Plain-text patterns: a header naming the inputs and scan cells, then one
// line of 0/1 per pattern.
void write_patterns(std::ostream& out, const Netlist& nl, const std::vector<TestCube>& patterns);

}  // namespace rith
