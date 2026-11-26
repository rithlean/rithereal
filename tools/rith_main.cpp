// SPDX-License-Identifier: Apache-2.0
// rith: command-line front end.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "rith/io.hpp"
#include "rith/fault.hpp"
#include "rith/sim.hpp"
#include "rith/testability.hpp"

namespace {

constexpr const char* kUsage =
    "rith - open-source design-for-test tools\n"
    "\n"
    "usage:\n"
    "  rith [--lib cells.lib ...] stats   <netlist>          print size, depth and gate mix\n"
    "  rith [--lib cells.lib ...] convert <in> <out>         convert between .bench and .v\n"
    "  rith [--lib cells.lib ...] equiv   <a> <b> [blocks]   random-simulation equivalence (full-scan view)\n"
    "  rith [--lib cells.lib ...] faultsim <netlist> [--patterns N] [--seed S] [--show-undetected N]\n"
    "                                                      random-pattern stuck-at fault simulation\n"
    "  rith [--lib cells.lib ...] testability <netlist> [--top N]\n"
    "                                                      SCOAP/COP; lists the hardest-to-test nets\n"
    "\n"
    "Formats are chosen by extension: .bench, .v. Use --lib (repeatable) for netlists\n"
    "mapped to a cell library, such as Design Compiler or Genus output.\n";

const rith::Library* g_lib = nullptr;

rith::Netlist load(const std::string& path) {
  auto nl = rith::read_netlist_file(path, g_lib);
  for (const auto& w : nl.warnings) std::cerr << "warning: " << path << ": " << w << "\n";
  return nl;
}

int stats(const std::string& path) {
  const auto nl = load(path);
  std::cout << "design   " << nl.name << "\n"
            << "inputs   " << nl.inputs().size() << "\n"
            << "outputs  " << nl.outputs().size() << "\n"
            << "flops    " << nl.flops().size() << "\n"
            << "gates    " << nl.size() << "\n"
            << "depth    " << nl.depth() << "\n";
  for (auto t : {rith::GateType::Buf, rith::GateType::Not, rith::GateType::And, rith::GateType::Nand,
                 rith::GateType::Or, rith::GateType::Nor, rith::GateType::Xor, rith::GateType::Xnor,
                 rith::GateType::Mux2, rith::GateType::Const0, rith::GateType::Const1}) {
    if (auto c = nl.count(t)) std::cout << "  " << rith::to_string(t) << "\t" << c << "\n";
  }
  return 0;
}

int convert(const std::string& in, const std::string& out) {
  const auto nl = load(in);
  rith::write_netlist_file(out, nl);
  std::cout << "wrote " << out << " (" << nl.size() << " gates)\n";
  return 0;
}

int equiv(const std::string& a, const std::string& b, std::size_t blocks) {
  const auto na = load(a);
  const auto nb = load(b);
  const auto r = rith::compare_by_simulation(na, nb, blocks);
  if (r.equivalent) {
    std::cout << "no difference found in " << r.patterns << " random patterns\n";
    return 0;
  }
  std::cout << "DIFFERENT: " << r.mismatch << " (after up to " << r.patterns << " patterns)\n";
  return 1;
}

// Reads "--name value" options after the positional arguments.
std::size_t option(const std::vector<std::string>& args, const std::string& name, std::size_t fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == name) return std::stoull(args[i + 1]);
  return fallback;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int faultsim(const std::vector<std::string>& args) {
  const auto nl = load(args[1]);
  const std::size_t patterns = option(args, "--patterns", 10000);
  const std::size_t seed = option(args, "--seed", 1);
  const std::size_t show = option(args, "--show-undetected", 0);
  const auto t0 = std::chrono::steady_clock::now();
  auto fl = rith::build_fault_list(nl);
  const std::size_t uncollapsed = fl.uncollapsed, unused = fl.unused;
  rith::FaultSimulator sim(nl, std::move(fl));
  const auto r = rith::run_random_patterns(sim, patterns, seed);
  const double secs = seconds_since(t0);
  std::printf("design        %s\n", nl.name.c_str());
  std::printf("faults        %zu collapsed (%zu uncollapsed, %zu in unobservable logic left out)\n", r.faults,
              uncollapsed, unused);
  std::printf("patterns      %zu random%s\n", r.patterns,
              r.patterns < patterns && r.detected < r.faults ? " (stopped: no new detections)" : "");
  std::printf("detected      %zu\n", r.detected);
  std::printf("coverage      %.2f%% collapsed, %.2f%% uncollapsed\n", 100 * r.coverage,
              100 * r.coverage_uncollapsed);
  std::printf("time          %.3f s\n", secs);
  std::size_t shown = 0;
  for (std::size_t i = 0; i < r.faults && shown < show; ++i)
    if (sim.status(i) == rith::FaultStatus::Undetected) {
      if (!shown++) std::printf("undetected:\n");
      std::printf("  %s\n", rith::describe(nl, sim.fault_list().faults[i]).c_str());
    }
  return 0;
}

int testability(const std::vector<std::string>& args) {
  const auto nl = load(args[1]);
  const std::size_t top = option(args, "--top", 20);
  const auto s = rith::compute_scoap(nl);
  const auto c = rith::compute_cop(nl);
  // Rank nets by their harder stem fault under random patterns (COP detection probability).
  // Nets nothing can observe are reported as a count; listing them is not useful.
  std::vector<std::pair<double, rith::GateId>> rank;
  std::size_t unobservable = 0;
  for (rith::GateId g = 0; g < nl.size(); ++g) {
    const auto t = nl.gate(g).type;
    if (t == rith::GateType::Const0 || t == rith::GateType::Const1) continue;
    if (s.co[g] >= rith::kScoapInf) {
      ++unobservable;
      continue;
    }
    rank.emplace_back(std::min(c.detect_sa0(g), c.detect_sa1(g)), g);
  }
  std::sort(rank.begin(), rank.end());
  auto fmt = [](std::uint32_t v) { return v >= rith::kScoapInf ? std::string("inf") : std::to_string(v); };
  std::printf("%zu hardest nets to test with random patterns (lowest COP detection probability first)\n",
              std::min(top, rank.size()));
  std::printf("%zu nets reach no output or flop and are not listed\n\n", unobservable);
  std::printf("%-28s %6s %6s %6s %9s %9s %10s\n", "net", "CC0", "CC1", "CO", "P(1)", "P(obs)", "P(detect)");
  for (std::size_t i = 0; i < rank.size() && i < top; ++i) {
    const auto g = rank[i].second;
    std::printf("%-28s %6s %6s %6s %9.4f %9.4f %10.2e\n", nl.gate(g).name.c_str(), fmt(s.cc0[g]).c_str(),
                fmt(s.cc1[g]).c_str(), fmt(s.co[g]).c_str(), c.p1[g], c.obs[g], rank[i].first);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  try {
    rith::Library lib;
    while (args.size() >= 2 && args[0] == "--lib") {
      rith::merge_into(lib, rith::read_liberty_file(args[1]));
      args.erase(args.begin(), args.begin() + 2);
    }
    if (!lib.cells.empty()) g_lib = &lib;
    if (args.size() == 2 && args[0] == "stats") return stats(args[1]);
    if (args.size() == 3 && args[0] == "convert") return convert(args[1], args[2]);
    if ((args.size() == 3 || args.size() == 4) && args[0] == "equiv")
      return equiv(args[1], args[2], args.size() == 4 ? std::stoul(args[3]) : 256);
    if (args.size() >= 2 && args[0] == "faultsim") return faultsim(args);
    if (args.size() >= 2 && args[0] == "testability") return testability(args);
    std::cerr << kUsage;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
