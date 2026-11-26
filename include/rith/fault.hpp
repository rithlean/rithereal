// SPDX-License-Identifier: Apache-2.0
// Single stuck-at faults: fault list, equivalence collapsing, and a
// bit-parallel fault simulator (parallel-pattern single-fault propagation).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rith/eval.hpp"
#include "rith/netlist.hpp"

namespace rith {

// A fault sits on a gate's output (a stem, pin == kOutputPin) or on one of its
// input pins (a fanout branch). Branch faults exist only where the driving net
// fans out to more than one place; otherwise they equal the driver's stem fault.
struct Fault {
  static constexpr std::int32_t kOutputPin = -1;
  GateId gate = kNoGate;
  std::int32_t pin = kOutputPin;
  bool stuck_at_one = false;

  bool operator==(const Fault&) const = default;
};

struct FaultList {
  std::vector<Fault> faults;           // one representative per equivalence class
  std::vector<std::uint32_t> weight;   // number of uncollapsed faults each one stands for
  std::size_t uncollapsed = 0;         // total faults before collapsing
  std::size_t unused = 0;              // left out: faults in logic no output or flop can see
};

// Full-scan view: inputs and flop outputs are controllable, outputs and flop D
// inputs are observable. Faults on constants are left out, and so are faults in
// logic that reaches no output or flop (dead logic, clock and reset ports).
FaultList build_fault_list(const Netlist& nl, bool collapse = true);

// Human-readable name, e.g. "n12 SA0" or "n12 -> U5.in1 SA1".
std::string describe(const Netlist& nl, const Fault& f);

enum class FaultStatus : std::uint8_t { Undetected, Detected };

class FaultSimulator {
 public:
  // threads = 0 uses every hardware thread.
  FaultSimulator(const Netlist& nl, FaultList faults, unsigned threads = 0);
  ~FaultSimulator();
  FaultSimulator(const FaultSimulator&) = delete;
  FaultSimulator& operator=(const FaultSimulator&) = delete;

  // Simulates up to 64 patterns. pi[i] drives nl.inputs()[i], state[i] drives
  // nl.flops()[i]; bits outside `valid` are ignored. Detected faults are dropped.
  // Returns how many faults this block newly detected.
  std::size_t simulate_block(std::span<const Word> pi, std::span<const Word> state, Word valid = kAllOnes);

  // Detection mask of one fault for the last block's good values, without
  // dropping: bit k set means pattern k detects it. Used by tests and ATPG.
  Word detect_mask(const Fault& f);

  const Netlist& netlist() const { return nl_; }
  const FaultList& fault_list() const { return faults_; }
  FaultStatus status(std::size_t i) const { return status_[i]; }
  // Index of the first pattern that detected fault i (counted across blocks), or -1.
  std::int64_t first_detection(std::size_t i) const { return first_pattern_[i]; }
  std::size_t patterns_simulated() const { return patterns_; }
  std::size_t detected() const { return detected_; }
  std::size_t detected_weight() const { return detected_weight_; }
  // Collapsed and uncollapsed (weighted) fault coverage, 0..1.
  double coverage() const;
  double coverage_uncollapsed() const;

 private:
  // Per-thread scratch state for event-driven propagation.
  struct Worker {
    std::vector<Word> faulty;
    std::vector<char> queued;
    std::vector<std::vector<GateId>> buckets;  // by level
    std::vector<GateId> touched;
    std::size_t pending = 0;
  };
  Word detect_mask(Worker& w, const Fault& f) const;
  Word propagate(Worker& w, GateId start, Word start_value) const;
  void schedule_fanouts(Worker& w, GateId g) const;

  const Netlist& nl_;
  FaultList faults_;
  std::vector<FaultStatus> status_;
  std::vector<std::int64_t> first_pattern_;
  std::size_t patterns_ = 0, detected_ = 0, detected_weight_ = 0, total_weight_ = 0;
  std::vector<Word> good_;
  std::vector<char> observed_;
  std::vector<Worker> workers_;
};

struct RandomFaultSimResult {
  std::size_t patterns = 0;
  std::size_t detected = 0;
  std::size_t faults = 0;
  double coverage = 0;              // collapsed
  double coverage_uncollapsed = 0;  // weighted by class size
};

// Random-pattern fault simulation. Stops after max_patterns, when every fault
// is detected, or after `patience` blocks of 64 in a row detect nothing new
// (patience 0: never stop early). `fixed` holds inputs (index into
// nl.inputs()) at a constant value, such as a test-mode enable.
RandomFaultSimResult run_random_patterns(FaultSimulator& sim, std::size_t max_patterns,
                                         std::uint64_t seed = 1, std::size_t patience = 64,
                                         std::span<const std::pair<std::size_t, bool>> fixed = {});

}  // namespace rith
