// SPDX-License-Identifier: Apache-2.0
// Self-contained tests (no external framework, so the build needs no network).
#include <algorithm>
#include <bit>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <cmath>
#include <random>

#include "rith/atpg.hpp"
#include "rith/fault.hpp"
#include "rith/design.hpp"
#include "rith/io.hpp"
#include "rith/scan.hpp"
#include "rith/sim.hpp"
#include "rith/testability.hpp"
#include "rith/tpi.hpp"

namespace {

int g_failures = 0;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
  static std::vector<std::pair<std::string, std::function<void()>>> r;
  return r;
}
struct Register {
  Register(const char* n, std::function<void()> f) { registry().emplace_back(n, std::move(f)); }
};
#define TEST(name)                                  \
  void name();                                      \
  const Register reg_##name(#name, name);           \
  void name()
#define CHECK(cond)                                                                         \
  do {                                                                                      \
    if (!(cond)) {                                                                          \
      ++g_failures;                                                                         \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n"; \
    }                                                                                       \
  } while (0)
#define CHECK_THROWS(expr, needle)                                                         \
  do {                                                                                     \
    bool thrown = false;                                                                   \
    try { expr; } catch (const std::exception& e) {                                       \
      thrown = std::string(e.what()).find(needle) != std::string::npos;                    \
      if (!thrown) std::cerr << "  unexpected message: " << e.what() << "\n";              \
    }                                                                                      \
    if (!thrown) {                                                                         \
      ++g_failures;                                                                        \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": expected error containing '" \
                << needle << "'\n";                                                        \
    }                                                                                      \
  } while (0)

const std::string kData = RITH_TEST_DATA;
rith::Netlist load(const std::string& f) { return rith::read_netlist_file(kData + "/" + f); }

rith::Netlist bench_str(const std::string& s) {
  std::istringstream in(s);
  return rith::read_bench(in);
}
rith::Netlist verilog_str(const std::string& s) {
  std::istringstream in(s);
  return rith::read_verilog(in);
}
std::string to_verilog(const rith::Netlist& nl) {
  std::ostringstream o;
  rith::write_verilog(o, nl);
  return o.str();
}
std::string to_bench(const rith::Netlist& nl) {
  std::ostringstream o;
  rith::write_bench(o, nl);
  return o.str();
}

// ---------------------------------------------------------------------------

TEST(c17_structure) {
  const auto nl = load("c17.bench");
  CHECK(nl.inputs().size() == 5);
  CHECK(nl.outputs().size() == 2);
  CHECK(nl.flops().empty());
  CHECK(nl.size() == 11);
  CHECK(nl.count(rith::GateType::Nand) == 6);
  CHECK(nl.depth() == 3);
  CHECK(nl.level(nl.find("11")) == 1);
  CHECK(nl.level(nl.find("16")) == 2);
  CHECK(nl.fanouts(nl.find("11")).size() == 2);
  CHECK(nl.fanouts(nl.find("16")).size() == 2);
  // Topological order: every gate comes after its fanins.
  std::vector<int> pos(nl.size());
  for (std::size_t i = 0; i < nl.topo_order().size(); ++i) pos[nl.topo_order()[i]] = static_cast<int>(i);
  for (rith::GateId g = 0; g < nl.size(); ++g)
    for (auto f : nl.fanins(g)) CHECK(pos[f] < pos[g]);
}

TEST(c17_exhaustive_simulation) {
  const auto nl = load("c17.bench");
  // Pattern p (0..31) sets input i to bit i of p.
  std::vector<rith::Word> pi(5, 0);
  for (int p = 0; p < 32; ++p)
    for (int i = 0; i < 5; ++i)
      if (p >> i & 1) pi[i] |= rith::Word{1} << p;
  const auto v = rith::simulate(nl, pi, {});
  const auto o22 = v[nl.find("22")], o23 = v[nl.find("23")];
  for (int p = 0; p < 32; ++p) {
    const bool i1 = p & 1, i2 = p >> 1 & 1, i3 = p >> 2 & 1, i6 = p >> 3 & 1, i7 = p >> 4 & 1;
    const bool n10 = !(i1 && i3), n11 = !(i3 && i6), n16 = !(i2 && n11), n19 = !(n11 && i7);
    CHECK(static_cast<bool>(o22 >> p & 1) == !(n10 && n16));
    CHECK(static_cast<bool>(o23 >> p & 1) == !(n16 && n19));
  }
}

TEST(s27_flops_are_cut) {
  const auto nl = load("s27.bench");
  CHECK(nl.inputs().size() == 4);
  CHECK(nl.flops().size() == 3);
  CHECK(nl.outputs().size() == 1);
  for (auto f : nl.flops()) CHECK(nl.level(f) == 0);
  // G11 = NOR(G5, G9) feeds flop G6 and NOT G17 even though it is defined late.
  CHECK(nl.fanouts(nl.find("G11")).size() == 3);
}

TEST(bench_verilog_roundtrip_is_equivalent) {
  for (const char* f : {"c17.bench", "s27.bench", "small_ref.bench"}) {
    const auto a = load(f);
    const auto b = verilog_str(to_verilog(a));
    const auto r = rith::compare_by_simulation(a, b);
    CHECK(r.equivalent);
    if (!r.equivalent) std::cerr << "  " << f << ": " << r.mismatch << "\n";
  }
}

TEST(verilog_writer_is_idempotent) {
  for (const char* f : {"c17.bench", "s27.bench", "yosys_small.v", "primitives.v"}) {
    const auto once = to_verilog(load(f));
    const auto twice = to_verilog(verilog_str(once));
    CHECK(once == twice);
  }
}

TEST(yosys_generic_cells) {
  const auto nl = load("yosys_small.v");
  CHECK(nl.name == "small");
  CHECK(nl.inputs().size() == 5);  // clk, a[1], a[0], b, sel
  CHECK(nl.flops().size() == 1);
  CHECK(nl.gate(nl.flops()[0]).clock == nl.find("clk"));
  CHECK(nl.find("a[1]") != rith::kNoGate);
  CHECK(nl.outputs().size() == 3);  // q, y[1], y[0]
  const auto r = rith::compare_by_simulation(nl, load("small_ref.bench"));
  CHECK(r.equivalent);
  if (!r.equivalent) std::cerr << "  " << r.mismatch << "\n";
}

TEST(bench_repeated_output_is_ignored) {
  const auto nl = bench_str("INPUT(a)\nOUTPUT(y)\nOUTPUT(y)\ny = NOT(a)\n");
  CHECK(nl.outputs().size() == 1);
  CHECK(nl.warnings.size() == 1);
}

TEST(verilog_primitives_and_constants) {
  const auto nl = load("primitives.v");
  CHECK(nl.name == "prim");
  CHECK(nl.inputs().size() == 3);
  const auto ref = bench_str(
      "INPUT(a)\nINPUT(b)\nINPUT(c)\nOUTPUT(y)\nOUTPUT(z)\n"
      "n1 = NAND(a, b)\nn2 = XOR(n1, c)\ny = NOT(n2)\nz = OR(a, c)\n");
  CHECK(rith::compare_by_simulation(nl, ref).equivalent);
}

TEST(verilog_to_bench_roundtrip) {
  const auto a = load("yosys_small.v");
  const auto b = bench_str(to_bench(a));
  CHECK(rith::compare_by_simulation(a, b).equivalent);
}

TEST(escaped_identifiers) {
  CHECK(rith::verilog_identifier("n12") == "n12");
  CHECK(rith::verilog_identifier("22") == "\\22 ");
  CHECK(rith::verilog_identifier("data[3]") == "\\data[3] ");
  CHECK(rith::verilog_identifier("wire") == "\\wire ");
}

TEST(equivalence_finds_a_bug) {
  const auto a = bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\ny = AND(a, b)\n");
  const auto b = bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\ny = OR(a, b)\n");
  const auto r = rith::compare_by_simulation(a, b);
  CHECK(!r.equivalent);
  CHECK(r.mismatch == "output y");
}

TEST(errors_are_reported) {
  CHECK_THROWS(bench_str("INPUT(x)\nOUTPUT(a)\na = AND(b, x)\nb = NOT(a)\n"), "combinational loop");
  CHECK_THROWS(bench_str("INPUT(x)\nOUTPUT(a)\na = NOT(x)\na = BUF(x)\n"), "more than one driver");
  CHECK_THROWS(bench_str("INPUT(x)\nOUTPUT(a)\na = AND(x, ghost)\n"), "undriven net(s): ghost");
  CHECK_THROWS(bench_str("INPUT(x)\nOUTPUT(a)\na = FOO(x)\n"), "unknown gate type");
  CHECK_THROWS(bench_str("INPUT(x)\nOUTPUT(a)\na = NOT(x, x)\n"), "has 2 inputs");
  CHECK_THROWS(verilog_str("module m(input a, output y); always @(a) y = a; endmodule"), "behavioural");
  CHECK_THROWS(verilog_str("module m(input a, output y); AND2_X1 u1 (.A1(a), .A2(a), .ZN(y)); endmodule"),
               "unknown cell 'AND2_X1'");
  CHECK_THROWS(verilog_str("module m(input [1:0] a, output y); buf (y, a); endmodule"), "whole-bus");
  CHECK_THROWS(verilog_str("module a(); endmodule module b(); endmodule"), "more than one module");
}

rith::Library tiny_lib() { return rith::read_liberty_file(kData + "/tiny.lib"); }

TEST(liberty_cells) {
  const auto lib = tiny_lib();
  CHECK(lib.name == "tiny");
  CHECK(lib.cells.size() == 21);
  const auto* inv = lib.find("INV_X1");
  CHECK(inv && inv->pin("ZN") && inv->pin("ZN")->function == "!A");
  CHECK(inv && inv->pin("A")->dir == rith::LibPin::Dir::Input);
  const auto* dff = lib.find("DFF_X1");
  CHECK(dff && dff->flop && dff->flop->iq == "IQ" && dff->flop->clocked_on == "CK" && !dff->is_scan);
  const auto* sdff = lib.find("SDFF_X1");
  CHECK(sdff && sdff->is_scan);
  CHECK(sdff && sdff->pin("SE")->signal_type == "test_scan_enable");
  CHECK(sdff && sdff->pin("SI")->signal_type == "test_scan_in");
  CHECK(lib.find("DFFR_X1")->flop->clear == "!RN");
  CHECK(lib.find("DLH_X1")->has_latch);
}

TEST(liberty_function_precedence) {
  using Op = rith::LibExpr::Op;
  auto e = rith::parse_liberty_function("A ^ B & C");  // XOR binds tighter than AND
  CHECK(e.op == Op::And && e.args.size() == 2 && e.args[0].op == Op::Xor);
  e = rith::parse_liberty_function("A B | C'");  // juxtaposition is AND, ' is NOT
  CHECK(e.op == Op::Or && e.args[0].op == Op::And && e.args[1].op == Op::Not);
  e = rith::parse_liberty_function("!(A1 & A2 & A3)");
  CHECK(e.op == Op::Not && e.args[0].op == Op::And && e.args[0].args.size() == 3);
  e = rith::parse_liberty_function("1");
  CHECK(e.op == Op::Const1);
  CHECK_THROWS(rith::parse_liberty_function("(A & B"), "missing ')'");
}

TEST(design_compiler_style_netlist) {
  const auto lib = tiny_lib();
  const auto nl = rith::read_netlist_file(kData + "/dc_style.v", &lib);
  CHECK(nl.name == "counter_dc");
  CHECK(nl.flops().size() == 3);
  CHECK(nl.find("cnt[0]") != rith::kNoGate && nl.gate(nl.find("cnt[0]")).type == rith::GateType::Dff);
  CHECK(nl.gate(nl.find("cnt[0]")).clock == nl.find("clk"));
  CHECK(nl.warnings.size() == 1 && nl.warnings[0].find("asynchronous") != std::string::npos);
  const auto r = rith::compare_by_simulation(nl, load("dc_style_ref.bench"));
  CHECK(r.equivalent);
  if (!r.equivalent) std::cerr << "  " << r.mismatch << "\n";
  // Mapped netlist -> generic Verilog -> back is still equivalent.
  const auto again = verilog_str(to_verilog(nl));
  CHECK(rith::compare_by_simulation(nl, again).equivalent);
}

TEST(liberty_errors) {
  const auto lib = tiny_lib();
  auto with_lib = [&](const std::string& src) {
    std::istringstream in(src);
    return rith::read_verilog(in, "<t>", &lib);
  };
  CHECK_THROWS(with_lib("module m(input d, input g, output q); DLH_X1 l (.D(d), .G(g), .Q(q)); endmodule"),
               "latch cell");
  CHECK_THROWS(with_lib("module m(input a, output y); NOR9_X1 u (.A(a), .ZN(y)); endmodule"),
               "not in the Liberty library");
  CHECK_THROWS(with_lib("module m(input a, output y); INV_X1 u (.X(a), .ZN(y)); endmodule"), "has no pin 'X'");
  CHECK_THROWS(with_lib("module m(input a, output y); NAND2_X1 u (.A1(a), .ZN(y)); endmodule"),
               "no connection on input A2");
}

// ---------------------------------------------------------------------------
// Phase 1: faults, fault simulation, testability
// ---------------------------------------------------------------------------

// Reference: simulate the whole circuit with the fault injected, then compare
// every observation point with the good circuit. Slow but obviously correct.
rith::Word brute_force_detect(const rith::Netlist& nl, const rith::Fault& f, const std::vector<rith::Word>& pi,
                              const std::vector<rith::Word>& st) {
  using namespace rith;
  const auto good = simulate(nl, pi, st);
  const Word stuck = f.stuck_at_one ? kAllOnes : 0;
  std::vector<Word> v(nl.size(), 0);
  for (std::size_t i = 0; i < pi.size(); ++i) v[nl.inputs()[i]] = pi[i];
  for (std::size_t i = 0; i < st.size(); ++i) v[nl.flops()[i]] = st[i];
  for (GateId g : nl.topo_order()) {
    const GateType t = nl.gate(g).type;
    if (t != GateType::Input && t != GateType::Dff) {
      const auto fi = nl.fanins(g);
      v[g] = eval_gate(t, fi.size(), [&](std::size_t i) {
        return (f.gate == g && f.pin == static_cast<int>(i)) ? stuck : v[fi[i]];
      });
    }
    if (f.gate == g && f.pin == Fault::kOutputPin) v[g] = stuck;
  }
  Word det = 0;
  for (const auto& [name, d] : nl.outputs()) det |= v[d] ^ good[d];
  for (GateId ff : nl.flops()) {
    const GateId d = nl.fanins(ff)[0];
    const Word seen = (f.gate == ff && f.pin == 0) ? stuck : v[d];
    det |= seen ^ good[d];
  }
  return det;
}

TEST(c17_fault_list) {
  const auto nl = load("c17.bench");
  const auto all = rith::build_fault_list(nl, false);
  CHECK(all.uncollapsed == 34);  // 11 stems + 6 fanout branches, two faults each
  CHECK(all.faults.size() == 34);
  const auto fl = rith::build_fault_list(nl);
  CHECK(fl.uncollapsed == 34);
  CHECK(fl.faults.size() == 22);  // the textbook collapsed count for c17
  std::size_t w = 0;
  for (auto x : fl.weight) w += x;
  CHECK(w == 34);
}

TEST(fault_simulator_matches_brute_force) {
  const auto lib = tiny_lib();
  std::vector<rith::Netlist> designs;
  designs.push_back(load("c17.bench"));
  designs.push_back(load("s27.bench"));
  designs.push_back(load("small_ref.bench"));
  designs.push_back(rith::read_netlist_file(kData + "/dc_style.v", &lib));
  std::mt19937_64 rng(7);
  for (const auto& nl : designs) {
    const auto fl = rith::build_fault_list(nl, false);
    rith::FaultSimulator sim(nl, fl);
    for (int block = 0; block < 4; ++block) {
      std::vector<rith::Word> pi(nl.inputs().size()), st(nl.flops().size());
      for (auto& w : pi) w = rng();
      for (auto& w : st) w = rng();
      sim.simulate_block(pi, st, 0);  // valid = 0: just load the good values, drop nothing
      for (const auto& f : fl.faults) {
        const auto fast = sim.detect_mask(f), slow = brute_force_detect(nl, f, pi, st);
        CHECK(fast == slow);
        if (fast != slow) std::cerr << "  " << nl.name << ": " << rith::describe(nl, f) << "\n";
      }
    }
  }
}

TEST(collapsed_faults_are_really_equivalent) {
  // Every fault in a class must have the same detection mask as its representative.
  const auto nl = load("s27.bench");
  const auto all = rith::build_fault_list(nl, false);
  const auto col = rith::build_fault_list(nl, true);
  rith::FaultSimulator sim(nl, all);
  std::mt19937_64 rng(3);
  std::vector<rith::Word> pi(nl.inputs().size()), st(nl.flops().size());
  for (auto& w : pi) w = rng();
  for (auto& w : st) w = rng();
  sim.simulate_block(pi, st, 0);
  std::size_t distinct = 0;
  std::vector<rith::Word> masks;
  for (const auto& f : all.faults) masks.push_back(sim.detect_mask(f));
  // Faults with identical masks are a superset of the equivalence classes, so
  // the number of distinct masks can never exceed the number of classes.
  std::sort(masks.begin(), masks.end());
  distinct = static_cast<std::size_t>(std::unique(masks.begin(), masks.end()) - masks.begin());
  CHECK(distinct <= col.faults.size());
  CHECK(col.faults.size() < all.faults.size());
}

TEST(c17_reaches_full_coverage) {
  const auto nl = load("c17.bench");
  rith::FaultSimulator sim(nl, rith::build_fault_list(nl));
  std::vector<rith::Word> pi(5, 0);  // all 32 patterns in one block
  for (int p = 0; p < 32; ++p)
    for (int i = 0; i < 5; ++i)
      if (p >> i & 1) pi[i] |= rith::Word{1} << p;
  sim.simulate_block(pi, {}, (rith::Word{1} << 32) - 1);
  CHECK(sim.detected() == 22);
  CHECK(sim.coverage() == 1.0);
  CHECK(sim.coverage_uncollapsed() == 1.0);
  CHECK(sim.patterns_simulated() == 32);
  for (std::size_t i = 0; i < 22; ++i) CHECK(sim.first_detection(i) >= 0 && sim.first_detection(i) < 32);
}

TEST(random_patterns_and_redundant_fault) {
  // y = a OR (a AND b) is just y = a, so the AND gate is redundant logic. Two fault
  // classes can never be seen: {t SA0, b SA0, a->t SA0} and {b SA1}.
  const auto nl = bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\nt = AND(a, b)\ny = OR(a, t)\n");
  rith::FaultSimulator sim(nl, rith::build_fault_list(nl));
  const auto r = rith::run_random_patterns(sim, 10000, 1, 8);
  CHECK(r.detected == r.faults - 2);
  CHECK(r.patterns < 10000);  // stopped early once nothing new was found
  std::vector<std::string> undetected;
  for (std::size_t i = 0; i < r.faults; ++i)
    if (sim.status(i) == rith::FaultStatus::Undetected)
      undetected.push_back(rith::describe(nl, sim.fault_list().faults[i]));
  std::sort(undetected.begin(), undetected.end());
  CHECK((undetected == std::vector<std::string>{"b SA0", "b SA1"}));  // b SA0 represents its class
}

TEST(unobservable_logic_is_left_out) {
  // In dc_style.v, clk and rst_n have no modelled pins and the half adder's S output
  // is unconnected, so faults there can never be seen and are counted as unused.
  const auto lib = tiny_lib();
  const auto nl = rith::read_netlist_file(kData + "/dc_style.v", &lib);
  const auto fl = rith::build_fault_list(nl);
  CHECK(fl.unused == 10);  // clk, rst_n, the S net, and the two branches feeding it: 5 x 2
  rith::FaultSimulator sim(nl, fl);
  const auto r = rith::run_random_patterns(sim, 100000, 1, 64);
  CHECK(r.detected == r.faults - 1);  // only the tie-high redundancy in the MUX remains
  for (std::size_t i = 0; i < r.faults; ++i)
    if (sim.status(i) == rith::FaultStatus::Undetected)
      CHECK(rith::describe(nl, sim.fault_list().faults[i]) == "U6/$1 SA1");
}

rith::Netlist random_netlist(std::size_t inputs, std::size_t gates, std::size_t flops, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::ostringstream b;
  std::vector<std::string> names;
  for (std::size_t i = 0; i < inputs; ++i) { b << "INPUT(i" << i << ")\n"; names.push_back("i" + std::to_string(i)); }
  for (std::size_t i = 0; i < flops; ++i) names.push_back("q" + std::to_string(i));
  const char* types[] = {"AND", "NAND", "OR", "NOR", "XOR", "XNOR", "NOT", "BUFF"};
  for (std::size_t k = 0; k < gates; ++k) {
    const std::string t = types[rng() % 8];
    const std::size_t window = std::min<std::size_t>(names.size(), 200);
    auto pick = [&] { return names[names.size() - 1 - rng() % window]; };
    b << "n" << k << " = " << t << "(" << pick();
    if (t != "NOT" && t != "BUFF") b << ", " << pick();
    b << ")\n";
    names.push_back("n" + std::to_string(k));
  }
  for (std::size_t i = 0; i < flops; ++i) b << "q" << i << " = DFF(n" << gates / 2 + rng() % (gates / 2) << ")\n";
  for (std::size_t k = gates - 20; k < gates; ++k) b << "OUTPUT(n" << k << ")\n";
  return bench_str(b.str());
}

TEST(multithreaded_fault_simulation_is_deterministic) {
  const auto nl = random_netlist(40, 3000, 60, 11);
  const auto fl = rith::build_fault_list(nl);
  CHECK(fl.faults.size() > 1000);  // enough faults to spread across threads
  rith::FaultSimulator one(nl, fl, 1), many(nl, fl, 4);
  const auto r1 = rith::run_random_patterns(one, 2048, 5);
  const auto r4 = rith::run_random_patterns(many, 2048, 5);
  CHECK(r1.detected == r4.detected);
  CHECK(r1.patterns == r4.patterns);
  for (std::size_t i = 0; i < fl.faults.size(); ++i) {
    CHECK(one.status(i) == many.status(i));
    CHECK(one.first_detection(i) == many.first_detection(i));
  }
}

TEST(scoap_c17) {
  const auto nl = load("c17.bench");
  const auto s = rith::compute_scoap(nl);
  auto id = [&](const char* n) { return nl.find(n); };
  CHECK(s.cc0[id("1")] == 1 && s.cc1[id("1")] == 1);
  CHECK(s.cc0[id("10")] == 3 && s.cc1[id("10")] == 2);  // NAND(1,3)
  CHECK(s.cc0[id("16")] == 4 && s.cc1[id("16")] == 2);  // NAND(2, 11)
  CHECK(s.co[id("22")] == 0 && s.co[id("23")] == 0);
  CHECK(s.co[id("10")] == 3);  // CO(22) + CC1(16) + 1
  CHECK(s.co[id("16")] == 3);  // min over both branches: 0 + CC1(10) + 1 = 3, 0 + CC1(19) + 1 = 3
}

TEST(scoap_flops_and_constants) {
  const auto nl = load("s27.bench");
  const auto s = rith::compute_scoap(nl);
  for (auto f : nl.flops()) CHECK(s.cc0[f] == 1 && s.cc1[f] == 1);
  CHECK(s.co[nl.find("G10")] == 0);  // drives a flop's D pin: a pseudo-output
  const auto k = bench_str("INPUT(a)\nOUTPUT(y)\nz = CONST0()\ny = AND(a, z)\n");
  const auto sk = rith::compute_scoap(k);
  CHECK(sk.cc1[k.find("y")] >= rith::kScoapInf);  // y can never be 1
  CHECK(sk.co[k.find("a")] >= rith::kScoapInf);   // a is never observable
}

TEST(cop_matches_exhaustive_probability) {
  // On a fanout-free circuit COP is exact, so compare it with brute force.
  const auto nl = bench_str(
      "INPUT(a)\nINPUT(b)\nINPUT(c)\nINPUT(d)\nOUTPUT(y)\n"
      "t = NAND(a, b)\nu = NOR(c, d)\ny = XOR(t, u)\n");
  const auto c = rith::compute_cop(nl);
  std::vector<rith::Word> pi(4, 0);
  for (int p = 0; p < 16; ++p)
    for (int i = 0; i < 4; ++i)
      if (p >> i & 1) pi[i] |= rith::Word{1} << p;
  const auto v = rith::simulate(nl, pi, {});
  const rith::Word m = 0xFFFF;
  for (rith::GateId g = 0; g < nl.size(); ++g) {
    const double exact = std::popcount(v[g] & m) / 16.0;
    CHECK(std::abs(c.p1[g] - exact) < 1e-12);
  }
  CHECK(std::abs(c.p1[nl.find("t")] - 0.75) < 1e-12);
  CHECK(std::abs(c.obs[nl.find("y")] - 1.0) < 1e-12);
  CHECK(std::abs(c.obs[nl.find("a")] - 0.5) < 1e-12);  // needs b = 1
  // Stem fault detection probability vs fault simulation over all 16 patterns.
  rith::FaultSimulator sim(nl, rith::build_fault_list(nl, false));
  sim.simulate_block(pi, {}, 0);
  for (const auto& f : sim.fault_list().faults) {
    if (f.pin != rith::Fault::kOutputPin) continue;
    const double exact = std::popcount(sim.detect_mask(f) & m) / 16.0;
    const double cop = f.stuck_at_one ? c.detect_sa1(f.gate) : c.detect_sa0(f.gate);
    CHECK(std::abs(cop - exact) < 1e-12);
  }
}

}  // namespace

int main() {
  int failed_tests = 0;
  for (const auto& [name, fn] : registry()) {
    const int before = g_failures;
    try {
      fn();
    } catch (const std::exception& e) {
      ++g_failures;
      std::cerr << "  uncaught exception: " << e.what() << "\n";
    }
    const bool ok = g_failures == before;
    failed_tests += !ok;
    std::cout << (ok ? "[ pass ] " : "[ FAIL ] ") << name << "\n";
  }
  std::cout << registry().size() - failed_tests << "/" << registry().size() << " tests passed\n";
  return failed_tests == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Structural view and scan insertion
// ---------------------------------------------------------------------------
namespace {

rith::Design design_file(const std::string& f) { return rith::read_verilog_design_file(kData + "/" + f); }
std::string design_text(const rith::Design& d) {
  std::ostringstream o;
  rith::write_verilog_design(o, d);
  return o.str();
}
rith::Design design_str(const std::string& s) {
  std::istringstream in(s);
  return rith::read_verilog_design(in);
}

}  // namespace

TEST(design_roundtrip_keeps_cells_and_buses) {
  const auto lib = tiny_lib();
  for (const char* f : {"dc_style.v", "scan_domains.v", "yosys_small.v", "primitives.v"}) {
    const auto d = design_file(f);
    const auto text = design_text(d);
    const auto again = design_str(text);
    CHECK(design_text(again) == text);  // writer is idempotent
    const auto a = rith::elaborate(d, &lib).netlist;
    const auto b = rith::elaborate(again, &lib).netlist;
    const auto r = rith::compare_by_simulation(a, b);
    CHECK(r.equivalent);
    if (!r.equivalent) std::cerr << "  " << f << ": " << r.mismatch << "\n";
  }
  const auto d = design_file("scan_domains.v");
  const auto text = design_text(d);
  CHECK(text.find("output [1:0] q;") != std::string::npos);
  CHECK(text.find("wire   [3:0] spare;") != std::string::npos);
  CHECK(text.find("DFFR_X1 \\r2_reg[0]  (") != std::string::npos);
  CHECK(text.find("assign q[1] = spare[2];") != std::string::npos);
  CHECK(d.ports.size() == 11 && d.ports[6].name == "bus[1]");
}

TEST(scan_cells_are_found_by_function) {
  const auto lib = tiny_lib();
  auto pins = rith::find_scan_pins(*lib.find("SDFF_X1"));
  CHECK(pins && pins->si == "SI" && pins->se == "SE" && pins->se_active_high);
  pins = rith::find_scan_pins(*lib.find("SDFFN_X1"));  // named SEN, active low, no signal_type
  CHECK(pins && pins->si == "SI" && pins->se == "SEN" && !pins->se_active_high);
  CHECK(!rith::find_scan_pins(*lib.find("DFF_X1")));
  auto eq = [&](const char* from) {
    auto e = rith::find_scan_equivalent(lib, *lib.find(from));
    return e ? e->cell->name : std::string("none");
  };
  CHECK(eq("DFF_X1") == "SDFF_X1");
  // DFFR_X1 has QN but SDFFR_X1 has not: fine only when QN is not used.
  CHECK(eq("DFFR_X1") == "none");
  const std::vector<std::string> only_q = {"Q"};
  const auto r_q = rith::find_scan_equivalent(lib, *lib.find("DFFR_X1"), &only_q);
  CHECK(r_q && r_q->cell->name == "SDFFR_X1");
  CHECK(eq("DFFN_X1") == "SDFFN_X1");
  CHECK(eq("DFFQN_X1") == "SDFFQN_X1");
  CHECK(eq("SDFF_X1") == "SDFF_X1");   // already a scan cell
  CHECK(eq("DFFE_X1") == "none");
}

TEST(scan_insertion_dc_style) {
  const auto lib = tiny_lib();
  auto d = design_file("dc_style.v");
  const auto before = rith::elaborate(d, &lib);
  const auto r = rith::insert_scan(d, lib);
  CHECK(r.chains.size() == 1);
  CHECK(r.scanned() == 3 && r.replaced == 2 && r.restitched == 1 && r.excluded.empty());
  CHECK(r.replaced_cells.at("DFF_X1") == "SDFF_X1" && r.replaced_cells.at("DFFR_X1") == "SDFFR_X1");
  const auto after = rith::elaborate(d, &lib);
  const auto m = rith::verify_mission_mode(before, after, r);
  CHECK(m.ok);
  if (!m.ok) std::cerr << "  " << m.message << "\n";
  const auto s = rith::verify_shift(after, r);
  CHECK(s.ok);
  if (!s.ok) std::cerr << "  " << s.message << "\n";
  // The written netlist reads back to the same thing.
  const auto again = rith::elaborate(design_str(design_text(d)), &lib);
  CHECK(rith::verify_shift(again, r).ok);
  CHECK(design_text(d).find("SDFFR_X1 \\cnt_reg[1]  ( .D(n3), .RN(rst_n), .CK(clk), .Q(\\cnt[1] ), .SI(") !=
        std::string::npos);
}

TEST(scan_insertion_clock_domains) {
  const auto lib = tiny_lib();
  auto d = design_file("scan_domains.v");
  const auto before = rith::elaborate(d, &lib);
  const auto r = rith::insert_scan(d, lib);
  // clk1 rising (r1, r2, r3, r9), clk1 falling (r4), clk2 falling (r5, clocked
  // through an inverter) and clk2 rising (r6). r7 has a gated clock and r8 has
  // no scan version.
  CHECK(r.chains.size() == 4);
  CHECK(r.scanned() == 7);
  CHECK(r.excluded.size() == 2);
  std::map<std::string, std::size_t> len;
  for (const auto& c : r.chains) len[c.clock + (c.falling_edge ? "-" : "+")] = c.cells.size();
  CHECK(len["clk1+"] == 4 && len["clk1-"] == 1 && len["clk2-"] == 1 && len["clk2+"] == 1);
  bool inverted = false;
  for (const auto& c : r.chains) inverted |= c.inverted();
  CHECK(inverted);  // r3 only has QN
  const auto after = rith::elaborate(d, &lib);
  CHECK(rith::verify_mission_mode(before, after, r).ok);
  const auto s = rith::verify_shift(after, r);
  CHECK(s.ok);
  if (!s.ok) std::cerr << "  " << s.message << "\n";
  // The active-low scan enable of SDFFN_X1 got an inverter.
  bool has_inv = false;
  for (const auto& i : d.instances) has_inv |= i.name == "rith_se_inv";
  CHECK(has_inv);
}

TEST(scan_chain_count_and_balance) {
  const auto lib = tiny_lib();
  {
    auto d = design_file("scan_domains.v");
    rith::ScanConfig cfg;
    cfg.chains = 6;
    const auto r = rith::insert_scan(d, lib, cfg);
    CHECK(r.chains.size() == 6);
    std::size_t longest = 0;
    for (const auto& c : r.chains) longest = std::max(longest, c.cells.size());
    CHECK(longest == 2);  // the 4 clk1 rising flops get the 2 spare chains: 2 + 1 + 1
    CHECK(rith::verify_shift(rith::elaborate(d, &lib), r).ok);
  }
  {
    auto d = design_file("scan_domains.v");
    rith::ScanConfig cfg;
    cfg.max_length = 3;
    const auto r = rith::insert_scan(d, lib, cfg);
    CHECK(r.chains.size() == 5);
  }
  {
    auto d = design_file("scan_domains.v");
    rith::ScanConfig cfg;
    cfg.chains = 3;
    CHECK_THROWS(rith::insert_scan(d, lib, cfg), "clock domains");
  }
  {
    auto d = design_file("scan_domains.v");
    rith::ScanConfig cfg;
    cfg.so_prefix = "y";  // "y1" is already an output
    CHECK_THROWS(rith::insert_scan(d, lib, cfg), "'y1' is already used");
  }
}

TEST(scan_checks_catch_a_broken_chain) {
  const auto lib = tiny_lib();
  auto d = design_file("scan_domains.v");
  const auto before = rith::elaborate(d, &lib);
  const auto r = rith::insert_scan(d, lib);
  const rith::ScanChain* longest = &r.chains[0];
  for (const auto& c : r.chains)
    if (c.cells.size() > longest->cells.size()) longest = &c;
  // Feed the third cell from the scan-in port: the second cell is skipped.
  auto broken = d;
  auto& third = broken.instances[longest->cells[2].instance];
  third.connect(longest->cells[2].si_pin, broken.find_net(longest->si_port));
  const auto s = rith::verify_shift(rith::elaborate(broken, &lib), r);
  CHECK(!s.ok && s.message.find("does not hold") != std::string::npos);
  // Driving a flop's D from the scan-in port changes mission mode.
  auto changed = d;
  auto& first = changed.instances[longest->cells[0].instance];
  first.connect("D", changed.find_net(longest->si_port));
  const auto m = rith::verify_mission_mode(before, rith::elaborate(changed, &lib), r);
  CHECK(!m.ok && m.message.find("next state of flop") != std::string::npos);
}

TEST(scan_outputs) {
  const auto lib = tiny_lib();
  auto d = design_file("dc_style.v");
  const auto r = rith::insert_scan(d, lib);
  std::ostringstream def, stil, dofile, proc, report;
  rith::write_def_scanchains(def, d, r);
  rith::write_stil_protocol(stil, d, r);
  rith::write_tessent_dofile(dofile, d, r, "counter_dc.testproc");
  rith::write_tessent_testproc(proc, d, r);
  rith::write_scan_report(report, d, r);
  CHECK(def.str().find("SCANCHAINS 1 ;") != std::string::npos);
  CHECK(def.str().find("cnt_reg\\[0\\] ( IN SI ) ( OUT Q )") != std::string::npos);
  CHECK(def.str().find("+ START PIN test_si1") != std::string::npos);
  CHECK(stil.str().find("ScanLength 3;") != std::string::npos);
  CHECK(stil.str().find("\"test_si1\" In { ScanIn; }") != std::string::npos);
  CHECK(dofile.str().find("add_scan_chains chain1 grp1 test_si1 test_so1") != std::string::npos);
  CHECK(proc.str().find("apply shift 3 ;") != std::string::npos);
  CHECK(report.str().find("DFF_X1 -> SDFF_X1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test point insertion
// ---------------------------------------------------------------------------
namespace {

// A circuit that random patterns struggle with: y = AND of 16 inputs feeds an
// XOR with more logic behind it, so faults behind the wide AND are rarely seen.
rith::Netlist hard_netlist() {
  std::ostringstream b;
  for (int i = 0; i < 20; ++i) b << "INPUT(i" << i << ")\n";
  b << "OUTPUT(y)\nOUTPUT(z)\n";
  b << "a0 = AND(i0, i1, i2, i3)\na1 = AND(i4, i5, i6, i7)\na2 = AND(i8, i9, i10, i11)\n"
       "a3 = AND(i12, i13, i14, i15)\nw = AND(a0, a1, a2, a3)\n"
       "x0 = XOR(i16, i17)\nx1 = AND(w, x0)\nx2 = OR(x1, i18)\ny = AND(x2, i19)\n"
       "z = NOR(a0, a2)\n";
  return bench_str(b.str());
}

}  // namespace

TEST(tpi_cop_matches_the_inserted_logic) {
  // The virtual COP of a netlist with test points must agree with plain COP on
  // the netlist that really contains them (enable tied to 1).
  const auto nl = random_netlist(30, 800, 20, 7);
  std::vector<rith::TestPoint> tps;
  std::mt19937_64 rng(3);
  for (int k = 0; k < 12; ++k) {
    rith::GateId g;
    do g = static_cast<rith::GateId>(rng() % nl.size());
    while (rith::is_source(nl.gate(g).type));
    tps.push_back({g, static_cast<rith::TpKind>(k % 3)});
  }
  const auto v = rith::compute_cop_with(nl, tps);
  const auto applied = rith::apply_test_points(nl, tps, "");
  const auto c = rith::compute_cop(applied);
  for (rith::GateId g = 0; g < nl.size(); ++g) {
    const rith::GateId a = applied.find(nl.gate(g).name);
    CHECK(a != rith::kNoGate);
    CHECK(std::abs(v.p1[g] - c.p1[a]) < 1e-9);
    CHECK(std::abs(v.obs[g] - c.obs[a]) < 1e-9);
  }
}

TEST(tpi_raises_random_pattern_coverage) {
  const auto nl = hard_netlist();
  rith::TpiConfig cfg;
  cfg.max_points = 4;
  cfg.patterns = 256;
  cfg.threads = 2;
  const auto r = rith::select_test_points(nl, cfg);
  CHECK(!r.points.empty() && r.points.size() <= 4);
  CHECK(r.cost.size() == r.points.size() + 1);
  CHECK(r.cost.back() < r.cost.front());
  const auto before = rith::random_coverage(nl, {}, 256, 5);
  const auto after = rith::random_coverage(nl, r.points, 256, 5);
  CHECK(before.faults == after.faults);  // the same original faults are counted
  CHECK(after.detected > before.detected);
  // Selection is deterministic whatever the thread count.
  cfg.threads = 1;
  CHECK(rith::select_test_points(nl, cfg).points == r.points);
}

TEST(tpi_skips_dead_logic) {
  // d drives nothing observable: no test point should go there.
  auto nl = bench_str(
      "INPUT(a)\nINPUT(b)\nINPUT(c)\nOUTPUT(y)\n"
      "d = AND(a, b, c)\ne = NOT(d)\ny = OR(a, b)\n");
  rith::TpiConfig cfg;
  cfg.patterns = 64;
  const auto r = rith::select_test_points(nl, cfg);
  for (const auto& tp : r.points) CHECK(nl.gate(tp.net).name != "d" && nl.gate(tp.net).name != "e");
  const auto cov = rith::random_coverage(nl, {}, 64);
  CHECK(cov.unused > 0 && cov.coverage() == 1.0);
}

TEST(tpi_fixed_inputs_hold_their_value) {
  // With b held at 0, nothing through the AND can be detected.
  const auto nl = bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\ny = AND(a, b)\n");
  rith::FaultSimulator sim(nl, rith::build_fault_list(nl, false));
  const std::pair<std::size_t, bool> fix[] = {{1, false}};
  const auto r = rith::run_random_patterns(sim, 256, 1, 0, fix);
  CHECK(r.patterns == 256);  // patience 0: no early stop
  for (std::size_t i = 0; i < r.faults; ++i) {
    const auto d = rith::describe(nl, sim.fault_list().faults[i]);
    const bool seen = sim.status(i) == rith::FaultStatus::Detected;
    CHECK(seen == (d == "y SA1" || d == "b SA1"));
  }
}

TEST(tpi_design_insertion_keeps_mission_mode) {
  const auto lib = tiny_lib();
  auto d = design_file("dc_style.v");
  const auto before = rith::elaborate(d, &lib);
  const std::vector<std::pair<std::string, rith::TpKind>> pts = {
      {"n4", rith::TpKind::Control1}, {"n6", rith::TpKind::Control0}, {"n5", rith::TpKind::Observe}};
  const auto ins = rith::insert_test_points(d, lib, pts);
  CHECK(ins.flops == 3);
  CHECK(ins.gates == 6);  // tiny.lib has no AND2/OR2: each control point is NAND2 + NAND2 + INV
  CHECK(ins.clock == "clk");
  const auto after = rith::elaborate(d, &lib);
  rith::ScanResult tie;
  tie.se_port = "tp_en";
  const auto m = rith::verify_mission_mode(before, after, tie);
  CHECK(m.ok);
  if (!m.ok) std::cerr << "  " << m.message << "\n";
  // The new flops then go on a scan chain like any other.
  const auto r = rith::insert_scan(d, lib);
  CHECK(r.scanned() == 6);
  CHECK(rith::verify_shift(rith::elaborate(d, &lib), r).ok);
  CHECK_THROWS(rith::insert_test_points(d, lib, pts), "tp_en");  // enable port already there
  auto fresh = design_file("dc_style.v");
  const auto w = rith::insert_test_points(fresh, lib, {{"no_such_net", rith::TpKind::Observe}});
  CHECK(w.flops == 0 && w.warnings.size() == 1);
}

// ---------------------------------------------------------------------------
// ATPG
// ---------------------------------------------------------------------------
namespace {

// A small random circuit with every gate type, including MUX and XOR.
rith::Netlist small_random_netlist(std::size_t inputs, std::size_t gates, std::size_t flops, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::ostringstream b;
  std::vector<std::string> names;
  for (std::size_t i = 0; i < inputs; ++i) { b << "INPUT(i" << i << ")\n"; names.push_back("i" + std::to_string(i)); }
  for (std::size_t i = 0; i < flops; ++i) names.push_back("q" + std::to_string(i));
  const char* types[] = {"AND", "NAND", "OR", "NOR", "XOR", "XNOR", "NOT", "BUFF", "MUX"};
  for (std::size_t k = 0; k < gates; ++k) {
    const std::string t = types[rng() % 9];
    auto pick = [&] { return names[names.size() - 1 - rng() % std::min<std::size_t>(names.size(), 12)]; };
    b << "n" << k << " = " << t << "(" << pick();
    const std::size_t extra = (t == "NOT" || t == "BUFF") ? 0 : t == "MUX" ? 2 : 1 + rng() % 2;
    for (std::size_t e = 0; e < extra; ++e) b << ", " << pick();
    b << ")\n";
    names.push_back("n" + std::to_string(k));
  }
  for (std::size_t i = 0; i < flops; ++i) b << "q" << i << " = DFF(n" << gates / 2 + rng() % (gates / 2) << ")\n";
  for (std::size_t k = gates - 4; k < gates; ++k) b << "OUTPUT(n" << k << ")\n";
  return bench_str(b.str());
}

// Whether each uncollapsed fault is testable, by trying every source assignment.
std::vector<bool> brute_force_testable(const rith::Netlist& nl, const rith::FaultList& fl) {
  const std::size_t pis = nl.inputs().size(), nfl = nl.flops().size(), n = pis + nfl;
  std::vector<bool> testable(fl.faults.size(), false);
  rith::FaultSimulator sim(nl, fl, 1);
  for (std::uint64_t base = 0; base < (std::uint64_t{1} << n); base += 64) {
    std::vector<rith::Word> pi(pis, 0), st(nfl, 0);
    for (std::uint64_t k = 0; k < 64 && base + k < (std::uint64_t{1} << n); ++k)
      for (std::size_t i = 0; i < n; ++i)
        if ((base + k) >> i & 1) (i < pis ? pi[i] : st[i - pis]) |= rith::Word{1} << k;
    sim.simulate_block(pi, st, 0);  // valid = 0: compute good values, drop nothing
    const std::uint64_t left = (std::uint64_t{1} << n) - base;
    const rith::Word valid = left >= 64 ? rith::kAllOnes : (rith::Word{1} << left) - 1;
    for (std::size_t i = 0; i < fl.faults.size(); ++i)
      if (sim.detect_mask(fl.faults[i]) & valid) testable[i] = true;
  }
  return testable;
}

// Does this cube (X filled with `fill`) detect the fault?
bool cube_detects(const rith::Netlist& nl, const rith::Fault& f, const rith::TestCube& c, std::uint64_t fill) {
  const std::size_t pis = nl.inputs().size(), nfl = nl.flops().size();
  std::vector<rith::Word> pi(pis, 0), st(nfl, 0);
  for (std::size_t i = 0; i < c.size(); ++i) {
    const bool v = c[i] < 0 ? (fill >> (i % 64) & 1) : c[i] == 1;
    if (v) (i < pis ? pi[i] : st[i - pis]) = 1;
  }
  rith::FaultList one;
  one.faults = {f};
  one.weight = {1};
  rith::FaultSimulator sim(nl, one, 1);
  sim.simulate_block(pi, st, 0);
  return sim.detect_mask(f) & 1;
}

void check_podem_against_brute_force(const rith::Netlist& nl, const char* what) {
  const auto fl = rith::build_fault_list(nl, false);
  const auto testable = brute_force_testable(nl, fl);
  rith::Podem podem(nl);
  rith::PodemConfig cfg;
  cfg.backtrack_limit = 0;  // complete search: never abort
  for (std::size_t i = 0; i < fl.faults.size(); ++i) {
    const auto r = podem.generate(fl.faults[i], cfg);
    const bool ok = testable[i] ? r.outcome == rith::AtpgOutcome::Detected : r.outcome == rith::AtpgOutcome::Redundant;
    CHECK(ok);
    if (!ok) {
      std::cerr << "  " << what << ": " << rith::describe(nl, fl.faults[i]) << " is "
                << (testable[i] ? "testable" : "untestable") << " but PODEM says " << rith::to_string(r.outcome)
                << "\n";
      return;
    }
    if (r.outcome == rith::AtpgOutcome::Detected) {
      // Every X really is a don't care: any fill must detect the fault.
      for (std::uint64_t fill : {std::uint64_t{0}, ~std::uint64_t{0}, std::uint64_t{0x5555555555555555}}) {
        const bool d = cube_detects(nl, fl.faults[i], r.cube, fill);
        CHECK(d);
        if (!d) {
          std::cerr << "  " << what << ": cube for " << rith::describe(nl, fl.faults[i]) << " fails\n";
          return;
        }
      }
    }
  }
}

}  // namespace

TEST(podem_matches_brute_force_on_benchmarks) {
  check_podem_against_brute_force(load("c17.bench"), "c17");
  check_podem_against_brute_force(load("s27.bench"), "s27");
  // y = a OR (a AND b) has redundant faults.
  check_podem_against_brute_force(bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\nt = AND(a, b)\ny = OR(a, t)\n"), "redundant");
}

TEST(podem_matches_brute_force_on_random_circuits) {
  // Fuzzing: every gate type, reconvergence, flops. Any disagreement is a bug
  // in PODEM or in the fault simulator.
  for (std::uint64_t seed = 1; seed <= 150; ++seed) {
    const auto nl = small_random_netlist(4 + seed % 6, 20 + seed % 40, seed % 4, seed);
    const std::string what = "random circuit seed " + std::to_string(seed);
    const int before = g_failures;
    check_podem_against_brute_force(nl, what.c_str());
    if (g_failures != before) return;
  }
}

TEST(podem_decides_only_on_inputs_and_traces) {
  const auto nl = load("c17.bench");
  rith::Podem podem(nl);
  rith::PodemConfig cfg;
  cfg.trace = true;
  const auto fl = rith::build_fault_list(nl, false);
  for (const auto& f : fl.faults) {
    const auto r = podem.generate(f, cfg);
    CHECK(r.outcome == rith::AtpgOutcome::Detected);  // c17 has no redundant faults
    CHECK(!r.trace.empty() && r.trace.front() == "fault " + rith::describe(nl, f) && r.trace.back() == "detected");
    for (const auto& line : r.trace)
      if (line.rfind("backtrace -> ", 0) == 0) {
        const auto name = line.substr(13, line.find('=') - 13);
        CHECK(nl.gate(nl.find(name)).type == rith::GateType::Input);
      }
    CHECK(r.cube.size() == nl.inputs().size());
  }
}

TEST(atpg_flow_on_benchmarks) {
  for (const char* f : {"c17.bench", "s27.bench"}) {
    const auto nl = load(f);
    const auto r = rith::run_atpg(nl);
    CHECK(r.errors.empty());
    CHECK(r.aborted == 0 && r.redundant == 0 && r.detected == r.faults);
    CHECK(r.efficiency() == 1.0);
    // Re-simulating the final pattern set independently detects every fault.
    rith::FaultSimulator sim(nl, rith::build_fault_list(nl), 1);
    const std::size_t pis = nl.inputs().size(), nfl = nl.flops().size();
    std::vector<rith::Word> pi(pis, 0), st(nfl, 0);
    CHECK(!r.patterns.empty() && r.patterns.size() <= 64);
    for (std::size_t k = 0; k < r.patterns.size(); ++k)
      for (std::size_t i = 0; i < pis + nfl; ++i)
        if (r.patterns[k][i] == 1) (i < pis ? pi[i] : st[i - pis]) |= rith::Word{1} << k;
    sim.simulate_block(pi, st, (rith::Word{1} << r.patterns.size()) - 1);
    CHECK(sim.detected() == r.faults);
  }
  // With no random phase, PODEM alone must reach the same result, and the
  // redundant faults of a OR (a AND b) are proved, not aborted.
  const auto red = bench_str("INPUT(a)\nINPUT(b)\nOUTPUT(y)\nt = AND(a, b)\ny = OR(a, t)\n");
  rith::AtpgConfig cfg;
  cfg.random_patterns = 0;
  const auto r = rith::run_atpg(red, cfg);
  CHECK(r.errors.empty() && r.redundant == 2 && r.aborted == 0 && r.efficiency() == 1.0 && r.coverage() < 1.0);
}
