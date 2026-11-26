// SPDX-License-Identifier: Apache-2.0
// rith: command-line front end.
#include <iostream>
#include <string>
#include <vector>

#include "rith/io.hpp"
#include "rith/sim.hpp"

namespace {

constexpr const char* kUsage =
    "rith - open-source design-for-test tools\n"
    "\n"
    "usage:\n"
    "  rith stats   <netlist>             print size, depth and gate mix\n"
    "  rith convert <in> <out>            convert between .bench and .v\n"
    "  rith equiv   <a> <b> [blocks]      random-simulation equivalence check (full-scan view)\n"
    "\n"
    "Formats are chosen by extension: .bench, .v\n";

int stats(const std::string& path) {
  const auto nl = rith::read_netlist_file(path);
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
  const auto nl = rith::read_netlist_file(in);
  rith::write_netlist_file(out, nl);
  std::cout << "wrote " << out << " (" << nl.size() << " gates)\n";
  return 0;
}

int equiv(const std::string& a, const std::string& b, std::size_t blocks) {
  const auto na = rith::read_netlist_file(a);
  const auto nb = rith::read_netlist_file(b);
  const auto r = rith::compare_by_simulation(na, nb, blocks);
  if (r.equivalent) {
    std::cout << "no difference found in " << r.patterns << " random patterns\n";
    return 0;
  }
  std::cout << "DIFFERENT: " << r.mismatch << " (after up to " << r.patterns << " patterns)\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  try {
    if (args.size() == 2 && args[0] == "stats") return stats(args[1]);
    if (args.size() == 3 && args[0] == "convert") return convert(args[1], args[2]);
    if ((args.size() == 3 || args.size() == 4) && args[0] == "equiv")
      return equiv(args[1], args[2], args.size() == 4 ? std::stoul(args[3]) : 256);
    std::cerr << kUsage;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
