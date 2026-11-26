// SPDX-License-Identifier: Apache-2.0
// Bit-parallel logic simulation: 64 patterns per machine word.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rith/netlist.hpp"

namespace rith {

using Word = std::uint64_t;

// Simulates one block of 64 patterns over the combinational (full-scan) view.
// pi_words[i] drives netlist.inputs()[i]; state_words[i] drives netlist.flops()[i].
// Returns one word per gate, indexed by GateId.
std::vector<Word> simulate(const Netlist& nl, std::span<const Word> pi_words,
                           std::span<const Word> state_words);

struct EquivalenceResult {
  bool equivalent = true;
  std::size_t patterns = 0;
  std::string mismatch;  // first differing output or flop, if any
};

// Random-pattern equivalence check of two netlists in the full-scan view.
// Inputs, outputs and flops are matched by name. An input present in only one
// netlist is allowed if it has no fanout (for example a clock port).
// Finds most real differences quickly but is not a formal proof.
EquivalenceResult compare_by_simulation(const Netlist& a, const Netlist& b,
                                        std::size_t blocks = 256, std::uint64_t seed = 1);

}  // namespace rith
