// SPDX-License-Identifier: Apache-2.0
// Word-parallel evaluation of one gate: each bit of a Word is a separate pattern.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rith/netlist.hpp"

namespace rith {

using Word = std::uint64_t;
inline constexpr Word kAllOnes = ~Word{0};

// `in(i)` returns the Word on fanin pin i (0 <= i < n).
// Sources (inputs, flops) are not evaluated here; constants are.
template <class In>
inline Word eval_gate(GateType t, std::size_t n, In&& in) {
  switch (t) {
    case GateType::Const0: return 0;
    case GateType::Const1: return kAllOnes;
    case GateType::Buf: return in(0);
    case GateType::Not: return ~in(0);
    case GateType::And:
    case GateType::Nand: {
      Word r = kAllOnes;
      for (std::size_t i = 0; i < n; ++i) r &= in(i);
      return t == GateType::And ? r : ~r;
    }
    case GateType::Or:
    case GateType::Nor: {
      Word r = 0;
      for (std::size_t i = 0; i < n; ++i) r |= in(i);
      return t == GateType::Or ? r : ~r;
    }
    case GateType::Xor:
    case GateType::Xnor: {
      Word r = 0;
      for (std::size_t i = 0; i < n; ++i) r ^= in(i);
      return t == GateType::Xor ? r : ~r;
    }
    case GateType::Mux2: {
      const Word s = in(0);
      return (~s & in(1)) | (s & in(2));
    }
    default: return 0;
  }
}

}  // namespace rith
