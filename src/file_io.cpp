// SPDX-License-Identifier: Apache-2.0
#include <fstream>
#include <string>

#include "rith/io.hpp"

namespace rith {

namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
enum class Format { Bench, Verilog };
Format format_of(const std::string& path) {
  if (ends_with(path, ".bench")) return Format::Bench;
  if (ends_with(path, ".v") || ends_with(path, ".sv") || ends_with(path, ".vg")) return Format::Verilog;
  throw std::runtime_error("cannot tell the format of '" + path + "' (use .bench or .v)");
}
}  // namespace

Netlist read_netlist_file(const std::string& path, const Library* lib) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open '" + path + "'");
  if (format_of(path) == Format::Verilog) return read_verilog(in, path, lib);
  Netlist nl = read_bench(in, path);
  // BENCH has no design name; use the file name without directory or extension.
  auto base = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
  nl.name = base.substr(0, base.rfind('.'));
  return nl;
}

void write_netlist_file(const std::string& path, const Netlist& nl) {
  const Format f = format_of(path);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write '" + path + "'");
  if (f == Format::Bench) write_bench(out, nl);
  else write_verilog(out, nl);
}

}  // namespace rith
