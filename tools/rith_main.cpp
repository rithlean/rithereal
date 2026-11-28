// SPDX-License-Identifier: Apache-2.0
// rith: command-line front end.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "rith/design.hpp"
#include "rith/io.hpp"
#include "rith/scan.hpp"
#include "rith/atpg.hpp"
#include "rith/tpi.hpp"
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
    "  rith  --lib cells.lib ...  scan <netlist.v> -o <out.v> [--chains N | --max-length L]\n"
    "        [--se test_se] [--si-prefix test_si] [--so-prefix test_so]\n"
    "        [--def scan.def] [--stil scan.spf] [--tessent prefix] [--report file] [--no-verify]\n"
    "                                                      insert mux-D scan chains, keep cell names\n"
    "  rith [--lib cells.lib ...] tpi <netlist> [--points K] [--patterns N] [--seed S]\n"
    "        [--observe-only | --control-only] [--cop-only] [-o out] [--enable tp_en] [--clock CK]\n"
    "                                                      test point insertion (greedy, COP + fault sim)\n"
    "  rith [--lib cells.lib ...] atpg <netlist> [--backtracks N] [--random N] [--seed S] [--no-compact] [--plain]\n"
    "        [-o patterns.txt] [--show-aborted N] [--trace FAULT]\n"
    "                                                      stuck-at test generation (random + PODEM)\n"
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

std::string text_option(const std::vector<std::string>& args, const std::string& name, const std::string& fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == name) return args[i + 1];
  return fallback;
}

bool flag(const std::vector<std::string>& args, const std::string& name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

template <class F>
void write_file(const std::string& path, F&& f) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write '" + path + "'");
  f(out);
  std::cout << "wrote " << path << "\n";
}

int scan(const std::vector<std::string>& args) {
  if (!g_lib) throw std::runtime_error("scan needs the cell library: rith --lib cells.lib scan ...");
  const std::string in = args[1];
  const std::string out = text_option(args, "-o", "");
  if (out.empty()) throw std::runtime_error("scan needs an output netlist: -o out.v");
  rith::ScanConfig cfg;
  cfg.chains = option(args, "--chains", 0);
  cfg.max_length = option(args, "--max-length", 0);
  cfg.se_port = text_option(args, "--se", cfg.se_port);
  cfg.si_prefix = text_option(args, "--si-prefix", cfg.si_prefix);
  cfg.so_prefix = text_option(args, "--so-prefix", cfg.so_prefix);

  const auto t0 = std::chrono::steady_clock::now();
  auto d = rith::read_verilog_design_file(in);
  const auto before = rith::elaborate(d, g_lib, in);
  for (const auto& w : before.netlist.warnings) std::cerr << "warning: " << in << ": " << w << "\n";
  const auto r = rith::insert_scan(d, *g_lib, cfg);

  std::size_t longest = 0;
  for (const auto& c : r.chains) longest = std::max(longest, c.cells.size());
  std::printf("design        %s\n", d.name.c_str());
  std::printf("scan flops    %zu (%zu replaced, %zu already scan cells), %zu left off\n", r.scanned(), r.replaced,
              r.restitched, r.excluded.size());
  std::printf("chains        %zu, longest %zu\n", r.chains.size(), longest);
  for (const auto& [inst, why] : r.excluded) std::printf("  left off %s: %s\n", inst.c_str(), why.c_str());
  for (const auto& w : r.warnings) std::cerr << "warning: " << w << "\n";

  int status = 0;
  if (!flag(args, "--no-verify")) {
    const auto after = rith::elaborate(d, g_lib, out);
    const auto m = rith::verify_mission_mode(before, after, r);
    std::printf("check         mission mode %s", m.ok ? "unchanged" : "CHANGED: ");
    if (!m.ok) std::printf("%s", m.message.c_str());
    const auto s = rith::verify_shift(after, r);
    std::printf(", shift %s\n", s.ok ? "ok on every chain" : ("FAILED: " + s.message).c_str());
    if (!m.ok || !s.ok) status = 1;
  }
  std::printf("time          %.3f s\n", seconds_since(t0));
  rith::write_verilog_design_file(out, d);
  std::cout << "wrote " << out << "\n";
  if (auto p = text_option(args, "--def", ""); !p.empty())
    write_file(p, [&](std::ostream& o) { rith::write_def_scanchains(o, d, r); });
  if (auto p = text_option(args, "--stil", ""); !p.empty())
    write_file(p, [&](std::ostream& o) { rith::write_stil_protocol(o, d, r); });
  if (auto p = text_option(args, "--tessent", ""); !p.empty()) {
    const std::string proc = p + ".testproc";
    write_file(p + ".dofile", [&](std::ostream& o) { rith::write_tessent_dofile(o, d, r, proc); });
    write_file(proc, [&](std::ostream& o) { rith::write_tessent_testproc(o, d, r); });
  }
  if (auto p = text_option(args, "--report", ""); !p.empty())
    write_file(p, [&](std::ostream& o) { rith::write_scan_report(o, d, r); });
  return status;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int tpi(const std::vector<std::string>& args) {
  const std::string in = args[1];
  const bool design_mode = g_lib && (ends_with(in, ".v") || ends_with(in, ".vg") || ends_with(in, ".sv"));
  rith::Design d;
  rith::Netlist nl;
  std::vector<char> allowed;
  if (design_mode) {
    d = rith::read_verilog_design_file(in);
    nl = rith::elaborate(d, g_lib, in).netlist;
    // Test points go on real nets of the netlist, not inside cells.
    allowed.assign(nl.size(), 0);
    for (rith::GateId g = 0; g < nl.size(); ++g) allowed[g] = d.find_net(nl.gate(g).name) != rith::kNoNet;
  } else {
    nl = load(in);
  }
  for (const auto& w : nl.warnings) std::cerr << "warning: " << in << ": " << w << "\n";

  rith::TpiConfig cfg;
  cfg.max_points = option(args, "--points", 16);
  cfg.patterns = option(args, "--patterns", 32768);
  cfg.observe = !flag(args, "--control-only");
  cfg.control = !flag(args, "--observe-only");
  cfg.allowed = allowed;
  const std::size_t seed = option(args, "--seed", 1);
  cfg.seed = seed;
  if (flag(args, "--cop-only")) cfg.refine_every = 0;
  const std::string enable = text_option(args, "--enable", "tp_en");

  auto t0 = std::chrono::steady_clock::now();
  const auto r = rith::select_test_points(nl, cfg);
  const double select_s = seconds_since(t0);
  std::printf("design        %s (%zu gates, %zu flops)\n", nl.name.c_str(), nl.size(), nl.flops().size());
  const std::string how =
      cfg.refine_every ? ", re-aimed by fault sim every " + std::to_string(cfg.refine_every) + " points" : "";
  std::printf("test points   %zu chosen in %.2f s (greedy on COP%s; cost = expected escapes after %zu patterns)\n",
              r.points.size(), select_s, how.c_str(), cfg.patterns);
  std::printf("\n  #  kind        net                              expected escapes\n");
  std::printf("  -  -           -                                %.1f\n", r.cost[0]);
  for (std::size_t k = 0; k < r.points.size(); ++k)
    std::printf("%3zu  %-10s  %-32s %.1f\n", k + 1, std::string(rith::to_string(r.points[k].kind)).c_str(),
                nl.gate(r.points[k].net).name.c_str(), r.cost[k + 1]);

  t0 = std::chrono::steady_clock::now();
  const auto before = rith::random_coverage(nl, {}, cfg.patterns, seed, enable);
  const auto after = rith::random_coverage(nl, r.points, cfg.patterns, seed, enable);
  std::printf("\nfault simulation, %zu random patterns, faults of the original logic (%zu):\n", cfg.patterns,
              before.faults);
  std::printf("  before  %.2f%% (%zu undetected)\n", 100 * before.coverage(), before.faults - before.detected);
  std::printf("  after   %.2f%% (%zu undetected)   [%.2f s]\n", 100 * after.coverage(), after.faults - after.detected,
              seconds_since(t0));

  const std::string out = text_option(args, "-o", "");
  if (out.empty()) return 0;
  if (design_mode) {
    const auto el_before = rith::elaborate(d, g_lib, in);
    std::vector<std::pair<std::string, rith::TpKind>> pts;
    for (const auto& p : r.points) pts.emplace_back(nl.gate(p.net).name, p.kind);
    rith::TpDesignConfig dc;
    dc.enable = enable;
    dc.clock = text_option(args, "--clock", "");
    const auto ins = rith::insert_test_points(d, *g_lib, pts, dc);
    for (const auto& w : ins.warnings) std::cerr << "warning: " << w << "\n";
    rith::ScanResult tie;  // mission-mode check with the test point enable held at 0
    tie.se_port = enable;
    const auto m = rith::verify_mission_mode(el_before, rith::elaborate(d, g_lib, out), tie);
    std::printf("\ninserted      %zu flops and %zu gates, clocked by %s; mission mode (%s = 0) %s\n", ins.flops,
                ins.gates, ins.clock.c_str(), enable.c_str(), m.ok ? "unchanged" : ("CHANGED: " + m.message).c_str());
    rith::write_verilog_design_file(out, d);
    std::cout << "wrote " << out << " (run 'rith scan' on it to put the new flops on chains)\n";
    return m.ok ? 0 : 1;
  }
  rith::write_netlist_file(out, rith::apply_test_points(nl, r.points, enable));
  std::cout << "wrote " << out << "\n";
  return 0;
}

int atpg(const std::vector<std::string>& args) {
  const auto nl = load(args[1]);
  for (const auto& w : nl.warnings) std::cerr << "warning: " << args[1] << ": " << w << "\n";
  if (const auto t = text_option(args, "--trace", ""); !t.empty()) {
    // Trace PODEM on one fault, named as `describe` prints it, e.g. "n12 SA0".
    const auto fl = rith::build_fault_list(nl, false);
    for (const auto& f : fl.faults)
      if (rith::describe(nl, f) == t) {
        const bool learning = !flag(args, "--plain");
        rith::Podem podem(nl, learning);
        rith::PodemConfig pc;
        pc.trace = true;
        pc.learning = learning;
        pc.backtrack_limit = option(args, "--backtracks", 1000);
        const auto r = podem.generate(f, pc);
        for (const auto& line : r.trace) std::cout << line << "\n";
        std::printf("%zu decisions, %zu backtracks\n", r.decisions, r.backtracks);
        return 0;
      }
    throw std::runtime_error("no fault named '" + t + "' (use the form 'net SA0' or 'net -> gate.inK SA1')");
  }
  rith::AtpgConfig cfg;
  cfg.backtrack_limit = option(args, "--backtracks", cfg.backtrack_limit);
  cfg.random_patterns = option(args, "--random", cfg.random_patterns);
  cfg.seed = option(args, "--seed", 1);
  cfg.compact = !flag(args, "--no-compact");
  cfg.learning = !flag(args, "--plain");
  const std::size_t show = option(args, "--show-aborted", 0);
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = rith::run_atpg(nl, cfg);
  const double secs = seconds_since(t0);
  std::printf("design        %s (%zu gates, %zu inputs, %zu scan cells)\n", nl.name.c_str(), nl.size(),
              nl.inputs().size(), nl.flops().size());
  std::printf("faults        %zu collapsed, %zu uncollapsed\n", r.faults, r.uncollapsed);
  std::printf("detected      %zu\n", r.detected);
  std::printf("redundant     %zu (proved untestable)\n", r.redundant);
  std::printf("aborted       %zu (backtrack limit %zu)\n", r.aborted, cfg.backtrack_limit);
  std::printf("coverage      %.2f%% fault coverage, %.2f%% fault efficiency (uncollapsed)\n", 100 * r.coverage(),
              100 * r.efficiency());
  std::printf("patterns      %zu (%zu random and %zu PODEM before compaction)\n", r.patterns.size(),
              r.random_patterns_kept, r.deterministic_patterns);
  std::printf("backtracks    %zu\n", r.backtracks);
  std::printf("engine        %s\n", cfg.learning ? ("PODEM + SOCRATES learning (" + std::to_string(r.learned) +
                                                         " learned implications) + unique sensitization").c_str()
                                                  : "plain PODEM");
  std::printf("time          %.2f s\n", secs);
  for (const auto& e : r.errors) std::fprintf(stderr, "internal error: %s\n", e.c_str());
  std::size_t shown = 0;
  for (std::size_t i = 0; i < r.faults && shown < show; ++i)
    if (r.status[i] == rith::AtpgOutcome::Aborted) {
      if (!shown++) std::printf("aborted:\n");
      std::printf("  %s\n", rith::describe(nl, r.fault_list.faults[i]).c_str());
    }
  if (const auto out = text_option(args, "-o", ""); !out.empty()) {
    write_file(out, [&](std::ostream& o) { rith::write_patterns(o, nl, r.patterns); });
    std::cout << "wrote " << out << "\n";
  }
  return r.errors.empty() ? 0 : 1;
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
    if (args.size() >= 2 && args[0] == "scan") return scan(args);
    if (args.size() >= 2 && args[0] == "tpi") return tpi(args);
    if (args.size() >= 2 && args[0] == "atpg") return atpg(args);
    std::cerr << kUsage;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
