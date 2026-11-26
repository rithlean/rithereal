// SPDX-License-Identifier: Apache-2.0
// Self-contained tests (no external framework, so the build needs no network).
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rith/io.hpp"
#include "rith/sim.hpp"

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
  CHECK(lib.cells.size() == 15);
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
